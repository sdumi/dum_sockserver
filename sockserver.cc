#include <ctype.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <errno.h>

#include <netdb.h>

#include <iostream>
#include <set>
using namespace std;

// global variables
const long MAX_CLIENTS = 2;

// will create a socket and bind our server to that socket
// returns the socket
int initialize(int argc, char* argv[]);
void setNonBlocking(int socket);
int buildSelectList(int socket, fd_set& socks, set<int>& connectionList, int highsocket);
void readSocks(int socket, fd_set& sockets, set<int>& connectionList);
void handleNewConnection(int socket, set<int>& connectionList);
void dealWithData(int pos, set<int>& connectionList);
int sock_gets(int sockFD, char* str, size_t count);
int sock_puts(int sockFD, char* str);

//-------------------------------------------------------------------
//
int main(int argc, char* argv[])
{
  int sock;
  set<int>connectList;
  fd_set socks;
  int highsock;
  // timeout for select
  struct timeval timeout;
  // number of sockets ready for reading
  int readsocks;
  //sock is our listening socket:
  if ((sock = initialize(argc, argv)) == -1)
  {
    cout << "server: failed to initialize" << endl;
    return 1;
  }
  cout << "server: waiting for connections"<<endl;
  // Yay! everything OK so far...
  // set up a queue for incoming connections
  // 2nd param: maximum length for the queue of pending connections
  if (listen(sock, 42) == -1)
  {
    perror("listen");
    return 1;
  }
  // in the beggining we have only one socket: the one we're listening on
  // ==> this is our highest  socket so far
  highsock = sock;
  // no connections yet
  connectList.clear();

  // now finally we can enter:
  /////////// MAIN LOOP //////////////
  while (1)
  {
    // need to recreate the list of sockets we want to listen to
    // we'll send it to select method... which will return when one
    // socket has some info on it
    highsock = buildSelectList(sock, socks, connectList, highsock);
    timeout.tv_sec  = 10;
    timeout.tv_usec = 0;
    // now we call select:
    // 1st param: highest file descriptor + 1
    // 2nd param: the address of the fd_set which contains the sockets
    //   we're waiting to be readable (this includes our listening socket)
    // 3rd param: fd_set with sockets on which we want to know if we can write on.
    //   here: not used: 0
    // 4th param: not used here: 0. Sockets with out-of-band data
    // 5th param: a timeout: how long select() should block. NULL if we want to wait
    //   forever until something happens on a socket.
    readsocks = select(highsock + 1,
                       &socks,
                       (fd_set*)0,
                       (fd_set*)0,
                       &timeout);
    // select() returns the number of sockets that have something on them
    // select() modifies the fd_set parameters to contain the reason why select() woke up
    // ==> socks will contain the sockets we can read from
    //   this is why at the beginning of the while we recreate the select list: buildSelectList()
    if (readsocks < 0)
    {
      perror("select");
      exit(EXIT_FAILURE);
    }
    if (readsocks == 0)
    {
      // Ummm... nothing to read yet, select() timedout
      //   well, we're alive at least
      cout << ".";
      fflush(stdout);// do we really need to do this?
    }
    else
    {
      // Jackpot!
      readSocks(sock, socks, connectList);
    }
  }
  return 0;
}

int initialize(int argc, char* argv[])
{
  /////////// SETUP PHASE //////////////
  // ASCII version of the server port
  char* asciiPort;
  // The port number after conversion from asciiPort
  int port;
  // bind info structure
  // from <arpa/inet.h>
  struct sockaddr_in server_address;
  // Used so we can re-bind to our port while a previous
  // connection is still in TIME_WAIT state
  int reuse_addr = 1;
  // the socket we're using:
  int sock;
  struct addrinfo hints, *servinfo, *p;

  // make sure we got a port number as a parameter
  if (argc < 2)
  {
    cout << "Usage: " << argv[0] << " PORT" << endl;
    return -1;
  }

  int rv;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=SOCK_STREAM;
  hints.ai_flags=AI_PASSIVE;//use my IP
  // obtain a file descriptor for our "listening" socket
  // use getaddrinfo to retrieve our address
  if ((rv = getaddrinfo(NULL, argv[1],
                        &hints, &servinfo)) != 0)
  {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return -1;
  }

  for (p=servinfo; p!=NULL;p=p->ai_next)
  {
    if ((sock=socket(p->ai_family, p->ai_socktype,
                     p->ai_protocol)) == -1)
    {
      perror("server:socket");
      continue;
    }

    // we want to re-bind to socket without TIME_WAIT problems
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    // set the socket to non-blocking
    // use our own method:
    setNonBlocking(sock);

    // get the port number: given by user
    asciiPort = argv[1];
    // convert to int:
    // from sockhelp.h
    port = atoi(asciiPort);

    // bind our server to the address:
    if (bind(sock, p->ai_addr, p->ai_addrlen) == -1)
    {
      perror("bind");
      // remember that our socket was already initialized half a page up
      close(sock);
      exit(EXIT_FAILURE);
    }
    cout << "port= " << port << " socketFD= " << sock << " s_addr= " << server_address.sin_addr.s_addr<<endl;
    break;
  }
  if (p == NULL)
  {
    cout << "server: failed to bind" << endl;
    return -1;
  }
  return sock;
}

//-------------------------------------------------------------------
//
void setNonBlocking(int socket)
{
  int opts;

  opts = fcntl(socket, F_GETFL);
  if (opts < 0)
  {
    perror("fnctl(F_GETFL)");
    exit(EXIT_FAILURE);
  }
  opts = (opts | O_NONBLOCK);
  if (fcntl(socket, F_SETFL, opts) < 0)
  {
    perror("fcntl(F_SETFL)");
    exit(EXIT_FAILURE);
  }
  return;
}

//-------------------------------------------------------------------
//
int buildSelectList(int socket, fd_set& sockets, set<int>& connectionList, int highsocket)
{
  // start from scratch
  FD_ZERO(&sockets);

  // add our listening socket to the list
  FD_SET(socket, &sockets);

  // add the alive sockets to the list
  for (set<int>::iterator i = connectionList.begin(); i!= connectionList.end(); ++i)
  {
    FD_SET(*i, &sockets);
    if (*i > highsocket)
      highsocket = *i;
  }
  return highsocket;
}

//-------------------------------------------------------------------
//
void readSocks(int socket, fd_set& sockets, set<int>& connectionList)
{
  // first check if somebody tries to connect to us
  // this happens when our listening socket becomes readable
  // ==> it will be part of the fd_set (ie, socks)
  if (FD_ISSET(socket, &sockets))
    handleNewConnection(socket, connectionList);

  // see who else has something to say to us:
  for (set<int>::iterator i = connectionList.begin(); i != connectionList.end(); ++i)
  {
    if (FD_ISSET(*i, &sockets))
      dealWithData(*i, connectionList);
  }
}

//-------------------------------------------------------------------
//
void handleNewConnection(int socket, set<int>& connectionList)
{
  // socket file descriptor for incoming connections
  int connection;

  connection = accept(socket, NULL, NULL);
  if (connection < 0)
  {
    perror("accept");
    exit(EXIT_FAILURE);
  }
  setNonBlocking(connection);
  if (connectionList.size() >= MAX_CLIENTS)
  {
    cout << endl << "No room left for new client" << endl;
    sock_puts(connection, "Sorry, this server is too busy. Try again later! \r\n");
    close(connection);
    return;
  }
  // store the new connection
  connectionList.insert(connection);
  cout << endl << "Connection accepted: FD= " << connection << "; Total connections= " << connectionList.size() << endl;
}

//-------------------------------------------------------------------
//
void dealWithData(int _sock, set<int>& connectionList)
{
  // buffer for socket reads
  char buffer[1024];
  char *cur_char;

  if (sock_gets(_sock, buffer, 1024) < 0)
  {
    // connection closed
    // close this end and free up entry in the connectlist
    cout << endl << "Connection lost: FD= " << _sock << endl;
    close(_sock);
    connectionList.erase(_sock);
  }
  else
  {
    // we got some data
    cout << endl << "Received from " << _sock << " the message: " << endl << buffer << endl;
    sock_puts(_sock, "Hi mate!\n");
    cout << "Responded: " << "Hi mate!" << endl;
  }
}

//-------------------------------------------------------------------
//
#warning this is old code
int sock_gets(int sockFD, char* str, size_t count)
{
  int bytesRead;
  int totalCount = 0;
  char *curPos;
  char lastRead = 0;

  curPos = str;
  while(lastRead != 10)
  {
    bytesRead = read(sockFD, &lastRead, 1);
    if (bytesRead <= 0)
    {
      // the other side may have closed unexpectedly
      return -1;
    }
    if ((totalCount < count) && (lastRead != 10) && (lastRead != 13) )
    {
      curPos[0] = lastRead; // this is funny shit
      ++curPos;
      ++totalCount;
    }
  }
  if (count > 0)
  {
    curPos[0] = 0; //again the funny shit
  }
  return totalCount;
}

//-------------------------------------------------------------------
//
#warning this is old code
int sock_puts(int sockFD, char *str)
{
  size_t count = strlen(str);
  size_t bytesSent = 0;
  int thisWrite;

  while (bytesSent < count)
  {
    do
      thisWrite = write(sockFD, str, count - bytesSent);
    while ((thisWrite < 0) && (errno == EINTR));
    if (thisWrite <= 0)
      return thisWrite;
    bytesSent += thisWrite;
    str += thisWrite;
  }
  return count;
}
