/*
copyright: Boaz segev, 2016
license: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

// lib server is based off and requires the following libraries:
#include "lib-server.h"
#include "libbuffer.h"

// #include <ssl.h>
// sys includes
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

////////////////////////////////////////////////////////////////////////////////
// socket binding and server limits helpers
static int bind_server_socket(struct Server*);
static int set_non_blocking_socket(int fd);
static long srv_capacity(void);

// // async data sending buffer and helpers
// struct Buffer {
//   struct Buffer* next;
//   int fd;
//   void* data;
//   int len;
//   int sent;
//   unsigned notification : 1;
//   unsigned moved : 1;
//   unsigned final : 1;
//   unsigned urgent : 1;
//   unsigned destroy : 4;
//   unsigned rsv : 3;
// };

////////////////////////////////////////////////////////////////////////////////
// Task object containers

// the data-type for async messages
struct FDTask {
  struct FDTask* next;
  struct Server* server;
  int fd;
  void (*task)(struct Server* server, int fd, void* arg);
  void* arg;
  void (*fallback)(struct Server* server, int fd, void* arg);
};

// A self handling task structure
struct GroupTask {
  struct GroupTask* next;
  struct Server* server;
  int fd_origin;
  void (*task)(struct Server* server, int fd, void* arg);
  void* arg;
  void (*on_finished)(struct Server* server, int fd, void* arg);
  unsigned char fds[];
};

////////////////////////////////////////////////////////////////////////////////
// The server data object container
struct Server {
  // inherit the reactor (must be first in the struct, for pointer conformity).
  struct Reactor reactor;
  // a pointer to the server's settings object.
  struct ServerSettings* settings;
  // a pointer to the thread pool object (libasync).
  struct Async* async;
  // a mutex for server data integrity
  pthread_mutex_t lock;
  // maps each connection to it's protocol.
  struct Protocol* volatile* protocol_map;
  // Used to send the server data + sockfd number to any worker threads.
  // pointer offset calculations are used to calculate the sockfd.
  struct Server** server_map;
  // maps each udata with it's associated connection fd.
  void** udata_map;
  // reading hook maps
  ssize_t (**reading_hooks)(server_pt srv, int fd, void* buffer, size_t size);
  /// maps a connection's "busy" flag, preventing the same connection from
  /// running `on_data` on two threads. use busy[fd] to get the status of the
  /// flag.
  volatile char* busy;
  /// maps all connection's timeout values. use tout[fd] to get/set the
  /// timeout.
  unsigned char* tout;
  /// maps all connection's idle cycle count values. use idle[fd] to get/set
  /// the count.
  unsigned char* idle;
  /// a mutex for server data integrity
  pthread_mutex_t task_lock;
  /// fd task pool.
  struct FDTask* fd_task_pool;
  /// group task pool.
  struct GroupTask* group_task_pool;
  /// task(s) pool size
  size_t fd_task_pool_size;
  size_t group_task_pool_size;
  // a buffer map.
  void** buffer_map;
  // socket capacity
  long capacity;
  /// the last timeout review
  time_t last_to;
  /// the server socket
  int srvfd;
  /// the original process pid
  pid_t root_pid;
  /// the flag that tells the server to stop
  volatile char run;
};

////////////////////////////////////////////////////////////////////////////////
// Server API gateway

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// Server API declerations

////////////////////////////////////////////////////////////////////////////////
// Server settings and objects

/** returns the originating process pid */
static pid_t root_pid(struct Server* server);
/// allows direct access to the reactor object. use with care.
static struct Reactor* srv_reactor(struct Server* server);
/// allows direct access to the server's original settings. use with care.
static struct ServerSettings* srv_settings(struct Server* server);
/// returns the computed capacity for any server instance on the system.
static long srv_capacity(void);

////////////////////////////////////////////////////////////////////////////////
// Server actions

/** listens to a server with the following server settings (which MUST include
a default protocol). */
static int srv_listen(struct ServerSettings);
/// stops a specific server, closing any open connections.
static void srv_stop(struct Server* server);
/// stops any and all server instances, closing any open connections.
static void srv_stop_all(void);

// helpers
static void srv_cycle_core(server_pt server);
static int set_to_busy(server_pt server, int fd);
static void async_on_data(server_pt* p_server);
static void on_ready(struct Reactor* reactor, int fd);
static void on_shutdown(struct Reactor* reactor, int fd);
static void on_close(struct Reactor* reactor, int fd);
static void clear_conn_data(server_pt server, int fd);
static void accept_async(server_pt server);

// signal management
static void register_server(struct Server* server);
static void on_signal(int sig);

////////////////////////////////////////////////////////////////////////////////
// Socket settings and data

/** Returns true if a specific connection's protected callback is running.

Protected callbacks include only the `on_message` callback and tasks forwarded
to the connection using the `td_task` or `each` functions.
*/
static unsigned char is_busy(struct Server* server, int sockfd);
/// retrives the active protocol object for the requested file descriptor.
static struct Protocol* get_protocol(struct Server* server, int sockfd);
/// sets the active protocol object for the requested file descriptor.
static int set_protocol(struct Server* server,
                        int sockfd,
                        struct Protocol* new_protocol);
/** retrives an opaque pointer set by `set_udata` and associated with the
connection.

since no new connections are expected on fd == 0..2, it's possible to store
global data in these locations. */
static void* get_udata(struct Server* server, int sockfd);
/** Sets the opaque pointer to be associated with the connection. returns the
old pointer, if any. */
static void* set_udata(struct Server* server, int sockfd, void* udata);
/** Sets the timeout limit for the specified connectionl, in seconds, up to
255 seconds (the maximum allowed timeout count). */
static void set_timeout(struct Server* server,
                        int sockfd,
                        unsigned char timeout);

////////////////////////////////////////////////////////////////////////////////
// Socket actions

/** Attaches an existing connection (fd) to the server's reactor and protocol
management system, so that the server can be used also to manage connection
based resources asynchronously (i.e. database resources etc').
*/
static int srv_attach(struct Server* server,
                      int sockfd,
                      struct Protocol* protocol);
/** Closes the connection.

If any data is waiting to be written, close will
return immediately and the connection will only be closed once all the data
was sent. */
static void srv_close(struct Server* server, int sockfd);
/** Hijack a socket (file descriptor) from the server, clearing up it's
resources. The control of hte socket is totally relinquished.

This method will block until all the data in the buffer is sent before
releasing control of the socket. */
static int srv_hijack(struct Server* server, int sockfd);
/** Counts the number of connections for the specified protocol (NULL = all
protocols). */
static long srv_count(struct Server* server, char* service);
/// "touches" a socket, reseting it's timeout counter.
static void srv_touch(struct Server* server, int sockfd);

////////////////////////////////////////////////////////////////////////////////
// Read and Write

/**
Sets up the read/write hooks, allowing for transport layer extensions (i.e.
SSL/TLS) or monitoring extensions.
*/
void rw_hooks(
    server_pt srv,
    int sockfd,
    ssize_t (*reading_hook)(server_pt srv, int fd, void* buffer, size_t size),
    ssize_t (*writing_hook)(server_pt srv, int fd, void* data, size_t len));

/** Reads up to `max_len` of data from a socket. the data is stored in the
`buffer` and the number of bytes received is returned.

Returns -1 if an error was raised and the connection was closed.

Returns the number of bytes written to the buffer. Returns 0 if no data was
available.
*/
static ssize_t srv_read(server_pt srv, int fd, void* buffer, size_t max_len);
/** Copies & writes data to the socket, managing an asyncronous buffer.

returns 0 on success. success means that the data is in a buffer waiting to
be written. If the socket is forced to close at this point, the buffer will be
destroyed (never sent).

On error, returns -1 otherwise returns the number of bytes in `len`.
*/
static ssize_t srv_write(struct Server* server,
                         int sockfd,
                         void* data,
                         size_t len);
/** Writes data to the socket, moving the data's pointer directly to the buffer.

Once the data was written, `free` will be called to free the data's memory.

returns 0 on success. success means that the data is in a buffer waiting to
be written. If the socket is forced to close at this point, the buffer will be
destroyed (never sent).

On error, returns -1 otherwise returns the number of bytes in `len`.
*/
static ssize_t srv_write_move(struct Server* server,
                              int sockfd,
                              void* data,
                              size_t len);
/** Copies & writes data to the socket, managing an asyncronous buffer.

Each call to a `write` function considers it's data atomic (a single package).

The `urgent` varient will send the data as soon as possible, without distrupting
any data packages (data written using `write` will not be interrupted in the
middle).

returns 0 on success. success means that the data is in a buffer waiting to
be written. If the socket is forced to close at this point, the buffer will be
destroyed (never sent).

On error, returns -1 otherwise returns the number of bytes in `len`.
*/
static ssize_t srv_write_urgent(struct Server* server,
                                int sockfd,
                                void* data,
                                size_t len);
/** Writes data to the socket, moving the data's pointer directly to the buffer.

Once the data was written, `free` will be called to free the data's memory.

Each call to a `write` function considers it's data atomic (a single package).

The `urgent` varient will send the data as soon as possible, without distrupting
any data packages (data written using `write` will not be interrupted in the
middle).

returns 0 on success. success means that the data is in a buffer waiting to
be written. If the socket is forced to close at this point, the buffer will be
destroyed (never sent).

On error, returns -1 otherwise returns the number of bytes in `len`.
*/
static ssize_t srv_write_move_urgent(struct Server* server,
                                     int sockfd,
                                     void* data,
                                     size_t len);
/**
Sends a whole file as if it were a single atomic packet.

Once the file was sent, the `FILE *` will be closed using `fclose`.

The file will be buffered to the socket chunk by chunk, so that memory
consumption is capped at ~ 64Kb.
*/
static ssize_t srv_sendfile(struct Server* server, int sockfd, FILE* file);

////////////////////////////////////////////////////////////////////////////////
// Tasks + Async

/** Schedules a specific task to run asyncronously for each connection.
a NULL service identifier == all connections (all protocols). */
static int each(struct Server* server,
                char* service,
                void (*task)(struct Server* server, int fd, void* arg),
                void* arg,
                void (*fallback)(struct Server* server, int fd, void* arg));
/** Schedules a specific task to run for each connection. The tasks will be
 * performed sequencially, in a blocking manner. The method will only return
 * once all the tasks were completed. A NULL service identifier == all
 * connections (all protocols).
*/
static int each_block(struct Server* server,
                      char* service,
                      void (*task)(struct Server* server, int fd, void* arg),
                      void* arg);
/** Schedules a specific task to run asyncronously for a specific connection.

returns -1 on failure, 0 on success (success being scheduling or performing
the task).
*/
static int fd_task(struct Server* server,
                   int sockfd,
                   void (*task)(struct Server* server, int fd, void* arg),
                   void* arg,
                   void (*fallback)(struct Server* server, int fd, void* arg));
/** Runs an asynchronous task, IF threading is enabled (set the
`threads` to 1 (the default) or greater).

If threading is disabled, the current thread will perform the task and return.

Returns -1 on error or 0
on succeess.
*/
static int run_async(struct Server* self, void task(void*), void* arg);
/** Creates a system timer (at the cost of 1 file descriptor) and pushes the
timer to the reactor. The task will NOT repeat. Returns -1 on error or the
new file descriptor on succeess.

NOTICE: Do NOT create timers from within the on_close callback, as this might
block resources from being properly freed (if the timer and the on_close
object share the same fd number).
*/
static int run_after(struct Server* self,
                     long milliseconds,
                     void task(void*),
                     void* arg);
/** Creates a system timer (at the cost of 1 file descriptor) and pushes the
timer to the reactor. The task will repeat `repetitions` times. if
`repetitions` is set to 0, task will repeat forever. Returns -1 on error
or the new file descriptor on succeess.

NOTICE: Do NOT create timers from within the on_close callback, as this might
block resources from being properly freed (if the timer and the on_close
object share the same fd number).
*/
static int run_every(struct Server* self,
                     long milliseconds,
                     int repetitions,
                     void task(void*),
                     void* arg);

// Global Task helpers
static inline int perform_single_task(server_pt srv,
                                      int fd,
                                      void (*task)(server_pt server,
                                                   int fd,
                                                   void* arg),
                                      void* arg);
// FDTask helpers
struct FDTask* new_fd_task(server_pt srv);
void destroy_fd_task(server_pt srv, struct FDTask* task);
static void perform_fd_task(struct FDTask* task);

// GroupTask helpers
struct GroupTask* new_group_task(server_pt srv);
void destroy_group_task(server_pt srv, struct GroupTask* task);
static void perform_group_task(struct GroupTask* task);
void add_to_group_task(struct Server* server, int fd, void* arg);

////////////////////////////////////////////////////////////////////////////////
// The actual Server API access setup
// The following allows access to helper functions and defines a namespace
// for the API in this library.
const struct Server__API____ Server = {
    .reactor = srv_reactor,                      //
    .settings = srv_settings,                    //
    .capacity = srv_capacity,                    //
    .listen = srv_listen,                        //
    .stop = srv_stop,                            //
    .stop_all = srv_stop_all,                    //
    .is_busy = is_busy,                          //
    .get_protocol = get_protocol,                //
    .set_protocol = set_protocol,                //
    .get_udata = get_udata,                      //
    .set_udata = set_udata,                      //
    .set_timeout = set_timeout,                  //
    .attach = srv_attach,                        //
    .close = srv_close,                          //
    .hijack = srv_hijack,                        //
    .count = srv_count,                          //
    .touch = srv_touch,                          //
    .rw_hooks = rw_hooks,                        //
    .read = srv_read,                            //
    .write = srv_write,                          //
    .write_move = srv_write_move,                //
    .write_urgent = srv_write_urgent,            //
    .write_move_urgent = srv_write_move_urgent,  //
    .sendfile = srv_sendfile,                    //
    .each = each,                                //
    .each_block = each_block,                    //
    .fd_task = fd_task,                          //
    .run_async = run_async,                      //
    .run_after = run_after,                      //
    .run_every = run_every,                      //
    .root_pid = root_pid,                        //
};

////////////////////////////////////////////////////////////////////////////////
// timer protocol

// service name
static char* timer_protocol_name = "timer";
// a timer protocol, will be allocated for each timer.
struct TimerProtocol {
  // must be first for pointer compatability
  struct Protocol parent;
  void (*task)(void*);
  void* arg;
  // how many repeats?
  int repeat;
};

// the timer's on_data callback
static void tp_perform_on_data(struct Server* self, int fd) {
  struct TimerProtocol* timer =
      (struct TimerProtocol*)Server.get_protocol(self, fd);
  if (timer) {
    // set local data copy, to avoid race contitions related to `free`.
    void (*task)(void*) = (void (*)(void*))timer->task;
    void* arg = timer->arg;
    // perform the task
    if (task)
      task(arg);
    // reset the timer
    reactor_reset_timer(fd);
    // close the file descriptor
    if (timer->repeat < 0)
      return;
    if (timer->repeat == 0) {
      reactor_close((struct Reactor*)self, fd);
      return;
    }
    timer->repeat--;
  }
}

// the timer's on_close (cleanup)
static void tp_perform_on_close(struct Server* self, int fd) {
  // free the protocol object when it was "aborted" using `close`.
  struct TimerProtocol* timer =
      (struct TimerProtocol*)Server.get_protocol(self, fd);
  if (timer)
    free(timer);
}

// creates a new TimeProtocol object.
// use: TimerProtocol(task, arg, rep)
static struct TimerProtocol* TimerProtocol(void* task,
                                           void* arg,
                                           int repetitions) {
  struct TimerProtocol* tp = malloc(sizeof(struct TimerProtocol));

  *tp = (struct TimerProtocol){.parent.on_data = tp_perform_on_data,
                               .parent.on_close = tp_perform_on_close,
                               .parent.service = timer_protocol_name,
                               .task = task,
                               .arg = arg,
                               .repeat = repetitions - 1};
  return tp;
}

////////////////////////////////////////////////////////////////////////////////
// Server settings and objects

/** returns the originating process pid */
static pid_t root_pid(struct Server* server) {
  return server->root_pid;
}
/// allows direct access to the reactor object. use with care.
static struct Reactor* srv_reactor(struct Server* server) {
  return (struct Reactor*)server;
}
/// allows direct access to the server's original settings. use with care.
static struct ServerSettings* srv_settings(struct Server* server) {
  return server->settings;
}

////////////////////////////////////////////////////////////////////////////////
// Socket settings and data

/** Returns true if a specific connection's protected callback is running.

Protected callbacks include only the `on_message` callback and tasks forwarded
to the connection using the `td_task` or `each` functions.
*/
static unsigned char is_busy(struct Server* server, int sockfd) {
  return server->busy[sockfd];
}
/// retrives the active protocol object for the requested file descriptor.
static struct Protocol* get_protocol(struct Server* server, int sockfd) {
  return server->protocol_map[sockfd];
}
/// sets the active protocol object for the requested file descriptor.
static int set_protocol(struct Server* server,
                        int sockfd,
                        struct Protocol* new_protocol) {
  // before bothering with the mutex, make sure we have a valid connection.
  if (!server->protocol_map[sockfd]) {
    // fprintf(stderr,
    //         "ERROR: Cannot set a protocol for a disconnected socket.\n");
    return -1;
  }
  // on_close and set_protocol should never conflict.
  // we should also prevent the same thread from deadlocking
  // (i.e., when on_close tries to update the protocol)
  if (pthread_mutex_trylock(&(server->lock)))
    return -1;
  // review the connection's validity again (in proteceted state)
  if (!server->protocol_map[sockfd]) {
    pthread_mutex_unlock(&(server->lock));
    // fprintf(stderr,
    //         "ERROR: Cannot set a protocol for a disconnected socket.\n");
    return -1;
  }
  // set the new protocol
  server->protocol_map[sockfd] = new_protocol;
  // release the lock
  pthread_mutex_unlock(&(server->lock));
  // return 0 (no error)
  return 0;
}
/** retrives an opaque pointer set by `set_udata` and associated with the
connection.

since no new connections are expected on fd == 0..2, it's possible to store
global data in these locations. */
static void* get_udata(struct Server* server, int sockfd) {
  return server->udata_map[sockfd];
}
/** Sets the opaque pointer to be associated with the connection. returns the
old pointer, if any. */
static void* set_udata(struct Server* server, int sockfd, void* udata) {
  void* old = server->udata_map[sockfd];
  // pthread_mutex_lock(&(server->lock));
  server->udata_map[sockfd] = udata;
  // pthread_mutex_unlock(&(server->lock));
  return old;
}
/** Sets the timeout limit for the specified connectionl, in seconds, up to
255 seconds (the maximum allowed timeout count). */
static void set_timeout(server_pt server, int fd, unsigned char timeout) {
  server->tout[fd] = timeout;
}

////////////////////////////////////////////////////////////////////////////////
// Server actions & Core

// helper macros
#define _reactor_(server) ((struct Reactor*)(server))
#define _server_(reactor) ((server_pt)(reactor))
#define _protocol_(reactor, fd) (_server_(reactor)->protocol_map[(fd)])

// clears a connection's data
static void clear_conn_data(server_pt server, int fd) {
  server->protocol_map[fd] = 0;
  server->busy[fd] = 0;
  server->tout[fd] = 0;
  server->idle[fd] = 0;
  server->udata_map[fd] = NULL;
  server->reading_hooks[fd] = NULL;
  Buffer.clear(server->buffer_map[fd]);
}
// on_ready, handles async buffer sends
static void on_ready(struct Reactor* reactor, int fd) {
  if (Buffer.flush(_server_(reactor)->buffer_map[fd], fd) > 0)
    _server_(reactor)->idle[fd] = 0;
  if (_protocol_(reactor, fd) && _protocol_(reactor, fd)->on_ready)
    _protocol_(reactor, fd)->on_ready(_server_(reactor), fd);
}

/// called for any open file descriptors when the reactor is shutting down.
static void on_shutdown(struct Reactor* reactor, int fd) {
  // call the callback for the mentioned active(?) connection.
  if (_protocol_(reactor, fd) && _protocol_(reactor, fd)->on_shutdown)
    _protocol_(reactor, fd)->on_shutdown(_server_(reactor), fd);
}

// called when a file descriptor was closed (either locally or by the other
// party, when it's a socket or a pipe).
static void on_close(struct Reactor* reactor, int fd) {
  if (!_protocol_(reactor, fd))
    return;
  // file descriptors can be added from external threads (i.e. creating timers),
  // this could cause race conditions and data corruption,
  // (i.e., when on_close is releasing memory).
  int lck = 0;
  // this should preven the same thread from calling on_close recursively
  // (i.e., when using the Server.attach)
  if ((lck = pthread_mutex_trylock(&(_server_(reactor)->lock)))) {
    if (lck != EAGAIN)
      return;
    pthread_mutex_lock(&(_server_(reactor)->lock));
  }
  if (_protocol_(reactor, fd)) {
    if (_protocol_(reactor, fd)->on_close)
      _protocol_(reactor, fd)->on_close(_server_(reactor), fd);
    clear_conn_data(_server_(reactor), fd);
  }
  pthread_mutex_unlock(&(_server_(reactor)->lock));
}

// The busy flag is used to make sure that a single connection doesn't perform
// two "critical" tasks at the same time. Critical tasks are defined as the
// `on_message` callback and any user task requested by `each` or `fd_task`.
static int set_to_busy(struct Server* server, int sockfd) {
  static pthread_mutex_t locker = PTHREAD_MUTEX_INITIALIZER;

  if (!server->protocol_map[sockfd])
    return 0;
  pthread_mutex_lock(&locker);
  if (server->busy[sockfd] == 1) {
    pthread_mutex_unlock(&locker);
    return 0;
  }
  server->busy[sockfd] = 1;
  pthread_mutex_unlock(&locker);
  return 1;
}

// accepts new connections
static void accept_async(server_pt server) {
  static socklen_t cl_addrlen = 0;
  int client = 1;
  while (1) {
#ifdef SOCK_NONBLOCK
    client = accept4(server->srvfd, NULL, &cl_addrlen, SOCK_NONBLOCK);
    if (client <= 0)
      return;
#else
    client = accept(server->srvfd, NULL, &cl_addrlen);
    if (client <= 0)
      return;
    set_non_blocking_socket(client);
#endif
    // handle server overload
    if (client >= _reactor_(server)->maxfd) {
      if (server->settings->busy_msg)
        if (write(client, server->settings->busy_msg,
                  strlen(server->settings->busy_msg)) < 0)
          ;
      close(client);
      continue;
    }
    // attach the new client (performs on_close if needed)
    srv_attach(server, client, server->settings->protocol);
  }
}
// makes sure that the on_data callback isn't overlapping a previous on_data
static void async_on_data(server_pt* p_server) {
  // compute sockfd by comparing the distance between the original pointer and
  // the passed pointer's address.
  if (!(*p_server))
    return;
  int sockfd = p_server - (*p_server)->server_map;
  // if we get the handle, perform the task
  if (set_to_busy(*p_server, sockfd)) {
    struct Protocol* protocol = (*p_server)->protocol_map[sockfd];
    if (!protocol || !protocol->on_data)
      return;
    (*p_server)->idle[sockfd] = 0;
    protocol->on_data((*p_server), sockfd);
    // release the handle
    (*p_server)->busy[sockfd] = 0;
    return;
  }
  // we didn't get the handle, reschedule - but only if the connection is still
  // open.
  if ((*p_server)->protocol_map[sockfd])
    Async.run((*p_server)->async, (void (*)(void*))async_on_data, p_server);
}

static void on_data(struct Reactor* reactor, int fd) {
  // static socklen_t cl_addrlen = 0;
  if (fd == _server_(reactor)->srvfd) {
    // listening socket. accept connections.
    Async.run(_server_(reactor)->async, (void (*)(void*))accept_async, reactor);
  } else if (_protocol_(reactor, fd) && _protocol_(reactor, fd)->on_data) {
    _server_(reactor)->idle[fd] = 0;
    // clients, forward on.
    Async.run(_server_(reactor)->async, (void (*)(void*))async_on_data,
              &(_server_(reactor)->server_map[fd]));
  }
}

// calls the reactor's core and checks for timeouts.
// schedules it's own execution when done.
// shouldn't be called by more then a single thread at a time (NOT thread safe)
static void srv_cycle_core(server_pt server) {
  static size_t idle_performed = 0;
  int delta;  // we also use this for other things, but it's the TOut delta
  // review reactor events
  delta = reactor_review(_reactor_(server));
  if (delta < 0) {
    srv_stop(server);
    return;
  } else if (delta == 0) {
    if (server->settings->on_idle && idle_performed == 0)
      server->settings->on_idle(server);
    idle_performed = 1;
  } else
    idle_performed = 0;
  // timeout + local close management
  if (server->last_to != _reactor_(server)->last_tick) {
    // We use the delta with fuzzy logic (only after the first second)
    int delta = _reactor_(server)->last_tick - server->last_to;
    for (long i = 3; i <= _reactor_(server)->maxfd; i++) {
      if (server->protocol_map[i] && fcntl(i, F_GETFL) < 0 && errno == EBADF) {
        reactor_close(_reactor_(server), i);
      }
      if (server->tout[i]) {
        if (server->tout[i] > server->idle[i])
          server->idle[i] += server->idle[i] ? delta : 1;
        else {
          if (server->protocol_map[i] && server->protocol_map[i]->ping)
            server->protocol_map[i]->ping(server, i);
          else if (!server->busy[i] || server->idle[i] == 255)
            reactor_close(_reactor_(server), i);
        }
      }
    }
    // ready for next call.
    server->last_to = _reactor_(server)->last_tick;
    // double en = (float)clock()/CLOCKS_PER_SEC;
    // printf("timeout review took %f milliseconds\n", (en-st)*1000);
  }
  if (server->run &&
      Async.run(server->async, (void (*)(void*))srv_cycle_core, server)) {
    perror(
        "FATAL ERROR:"
        "couldn't schedule the server's reactor in the task queue");
    exit(1);
  }
}

/** listens to a server with the following server settings (which MUST include
a default protocol). */
static int srv_listen(struct ServerSettings settings) {
  // review the settings, setup defaults when something's missing
  if (!settings.protocol) {
    fprintf(stderr,
            "Server err: (protocol == NULL) Running a server requires "
            "a protocol for new connections.\n");
    return -1;
  }
  if (!settings.port)
    settings.port = "3000";
  if (!settings.timeout)
    settings.timeout = 5;
  if (!settings.threads || settings.threads <= 0)
    settings.threads = 1;
  if (!settings.processes || settings.processes <= 0)
    settings.processes = 1;

  // We can use the stack for the server's core memory.
  long capacity = srv_capacity();
  void* udata_map[capacity];
  struct Protocol* volatile protocol_map[capacity];
  struct Server* server_map[capacity];
  void* buffer_map[capacity];
  volatile char busy[capacity];
  unsigned char tout[capacity];
  unsigned char idle[capacity];
  ssize_t (*reading_hooks[capacity])(server_pt srv, int fd, void* buffer,
                                     size_t size);

  // populate the Server structure with the data
  struct Server srv = {
      // initialize the server object
      .settings = &settings,  // store a pointer to the settings
      .last_to = 0,           // last timeout review
      .capacity = capacity,   // the server's capacity
      .protocol_map = protocol_map,
      .udata_map = udata_map,
      .server_map = server_map,
      .buffer_map = buffer_map,
      .reading_hooks = reading_hooks,
      .busy = busy,
      .tout = tout,
      .idle = idle,
      .fd_task_pool = NULL,
      .fd_task_pool_size = 0,
      .group_task_pool = NULL,
      .group_task_pool_size = 0,
      .reactor.maxfd = capacity - 1,
      .reactor.on_data = on_data,
      .reactor.on_ready = on_ready,
      .reactor.on_shutdown = on_shutdown,
      .reactor.on_close = on_close,
  };
  // initialize the server data mutex
  if (pthread_mutex_init(&srv.lock, NULL)) {
    return -1;
  }
  // initialize the server task pool mutex
  if (pthread_mutex_init(&srv.task_lock, NULL)) {
    pthread_mutex_destroy(&srv.lock);
    return -1;
  }

  // bind the server's socket - if relevent
  int srvfd = 0;
  if (settings.port > 0) {
    srvfd = bind_server_socket(&srv);
    // if we didn't get a socket, quit now.
    if (srvfd < 0)
      return -1;
    srv.srvfd = srvfd;
  }

  // zero out data...
  for (int i = 0; i < capacity; i++) {
    protocol_map[i] = 0;
    server_map[i] = &srv;
    busy[i] = 0;
    tout[i] = 0;
    idle[i] = 0;
    udata_map[i] = 0;
    // buffer_map[i] = 0;
    buffer_map[i] = Buffer.new(&srv);
  }

  // register signals - do this before concurrency, so that they are inherited.
  struct sigaction old_term, old_int, old_pipe, new_int, new_pipe;
  sigemptyset(&new_int.sa_mask);
  sigemptyset(&new_pipe.sa_mask);
  new_pipe.sa_flags = new_int.sa_flags = 0;
  new_pipe.sa_handler = SIG_IGN;
  new_int.sa_handler = on_signal;
  sigaction(SIGINT, &new_int, &old_int);
  sigaction(SIGTERM, &new_int, &old_term);
  sigaction(SIGPIPE, &new_pipe, &old_pipe);

  // setup concurrency
  srv.root_pid = getpid();
  pid_t pids[settings.processes > 0 ? settings.processes : 0];
  if (settings.processes > 1) {
    pids[0] = 0;
    for (int i = 1; i < settings.processes; i++) {
      if (getpid() == srv.root_pid)
        pids[i] = fork();
    }
  }
  // once we forked, we can initiate a thread pool for each process
  srv.async = Async.new(settings.threads);
  if (srv.async <= 0) {
    if (srvfd)
      close(srvfd);
    return -1;
  }

  // register server for signals
  register_server(&srv);

  // let'm know...
  if (srvfd)
    printf(
        "(pid %d x %d threads) Listening on port %s (max sockets: %lu, ~%d "
        "used)\n",
        getpid(), srv.settings->threads, srv.settings->port, srv.capacity,
        srv.srvfd + 2);
  else
    printf(
        "(pid %d x %d threads) Started a non-listening network service"
        "(max sockets: %lu ~ at least 6 are in system use)\n",
        getpid(), srv.settings->threads, srv.capacity);

  // initialize reactor
  reactor_init(&srv.reactor);
  // bind server data to reactor loop
  if (srvfd)
    reactor_add(&srv.reactor, srv.srvfd);

  // call the on_init callback
  if (settings.on_init) {
    settings.on_init(&srv);
  }

  // initiate the core's cycle
  srv.run = 1;
  Async.run(srv.async, (void (*)(void*))srv_cycle_core, &srv);
  Async.wait(srv.async);
  fprintf(stderr, "server done\n");
  // cleanup
  reactor_stop(&srv.reactor);

  if (settings.processes > 1 && getpid() == srv.root_pid) {
    int sts;
    for (int i = 1; i < settings.processes; i++) {
      // printf("sending signal to pid %d\n", pids[i]);
      kill(pids[i], SIGINT);
    }
    for (int i = 1; i < settings.processes; i++) {
      sts = 0;
      // printf("waiting for pid %d\n", pids[i]);
      if (waitpid(pids[i], &sts, 0) != pids[i])
        perror("waiting for child process had failed");
    }
  }

  if (settings.on_finish)
    settings.on_finish(&srv);
  printf("\n(%d) Stopped listening for port %s\n", getpid(), settings.port);
  if (getpid() != srv.root_pid) {
    fflush(NULL);
    exit(0);
  }
  // restore signal state
  sigaction(SIGINT, &old_int, NULL);
  sigaction(SIGTERM, &old_term, NULL);
  sigaction(SIGPIPE, &old_pipe, NULL);

  // destroy the buffers.
  for (int i = 0; i < srv_capacity(); i++) {
    Buffer.destroy(buffer_map[i]);
  }
  // destroy the task pools
  destroy_fd_task(&srv, NULL);
  destroy_group_task(&srv, NULL);

  // destroy the mutexes
  pthread_mutex_destroy(&srv.lock);
  pthread_mutex_destroy(&srv.task_lock);

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Socket actions

/** Attaches an existing connection (fd) to the server's reactor and protocol
management system, so that the server can be used also to manage connection
based resources asynchronously (i.e. database resources etc').
*/
static int srv_attach(server_pt server, int sockfd, struct Protocol* protocol) {
  if (server->protocol_map[sockfd]) {
    on_close((struct Reactor*)server, sockfd);
  }
  if (sockfd >= server->capacity)
    return -1;
  // setup protocol
  server->protocol_map[sockfd] = protocol;  // set_protocol() would cost more
  // setup timeouts
  server->tout[sockfd] = server->settings->timeout;
  server->idle[sockfd] = 0;
  // call on_open
  // register the client - start it off as busy, protocol still initializing
  // we don't need the mutex, because it's all fresh
  server->busy[sockfd] = 1;
  // attach the socket to the reactor
  if (reactor_add((struct Reactor*)server, sockfd) < 0) {
    clear_conn_data(server, sockfd);
    return -1;
  }
  if (protocol->on_open)
    protocol->on_open(server, sockfd);
  server->busy[sockfd] = 0;
  return 0;
}
/** Closes the connection.

If any data is waiting to be written, close will
return immediately and the connection will only be closed once all the data
was sent. */
static void srv_close(struct Server* server, int sockfd) {
  if (!server->protocol_map[sockfd])
    return;
  if (Buffer.is_empty(server->buffer_map[sockfd])) {
    reactor_close((struct Reactor*)server, sockfd);
  } else
    Buffer.close_when_done(server->buffer_map[sockfd], sockfd);
}
/** Hijack a socket (file descriptor) from the server, clearing up it's
resources. The control of the socket is totally relinquished.

This method will block until all the data in the buffer is sent before
releasing control of the socket. */
static int srv_hijack(struct Server* server, int sockfd) {
  if (!server->protocol_map[sockfd])
    return -1;
  reactor_remove((struct Reactor*)server, sockfd);
  while (!Buffer.is_empty(server->buffer_map[sockfd]) &&
         Buffer.flush(server->buffer_map[sockfd], sockfd) >= 0)
    ;
  clear_conn_data(server, sockfd);
  return 0;
}
/** Counts the number of connections for the specified protocol (NULL = all
protocols). */
static long srv_count(struct Server* server, char* service) {
  int c = 0;
  if (service) {
    for (int i = 0; i < server->capacity; i++) {
      if (server->protocol_map[i] &&
          (server->protocol_map[i]->service == service ||
           !strcmp(server->protocol_map[i]->service, service)))
        c++;
    }
  } else {
    for (int i = 0; i < server->capacity; i++) {
      if (server->protocol_map[i] &&
          server->protocol_map[i]->service != timer_protocol_name)
        c++;
    }
  }
  return c;
}
/// "touches" a socket, reseting it's timeout counter.
static void srv_touch(struct Server* server, int sockfd) {
  server->idle[sockfd] = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Read and Write

/**
Sets up the read/write hooks, allowing for transport layer extensions (i.e.
SSL/TLS) or monitoring extensions.
*/
void rw_hooks(
    server_pt srv,
    int sockfd,
    ssize_t (*reading_hook)(server_pt srv, int fd, void* buffer, size_t size),
    ssize_t (*writing_hook)(server_pt srv, int fd, void* data, size_t len)) {
  srv->reading_hooks[sockfd] = reading_hook;
  Buffer.set_whook(srv->buffer_map[sockfd], writing_hook);
}

/** Reads up to `max_len` of data from a socket. the data is stored in the
`buffer` and the number of bytes received is returned.

Returns -1 if an error was raised and the connection was closed.

Returns the number of bytes written to the buffer. Returns 0 if no data was
available.
*/
static ssize_t srv_read(server_pt srv, int fd, void* buffer, size_t max_len) {
  if (srv->reading_hooks[fd])  // check for reading hook
    return srv->reading_hooks[fd](srv, fd, buffer, max_len);

  ssize_t read = 0;
  if ((read = recv(fd, buffer, max_len, 0)) > 0) {
    // reset timeout
    srv->idle[fd] = 0;
    // return data
    return read;
  } else {
    if (read && (errno & (EWOULDBLOCK | EAGAIN)))
      return 0;
  }
  return -1;
}
/** Copies & writes data to the socket, managing an asyncronous buffer.

On error, returns -1 otherwise returns the number of bytes already sent.
*/
static ssize_t srv_write(struct Server* server,
                         int sockfd,
                         void* data,
                         size_t len) {
  // make sure the socket is alive
  if (!server->protocol_map[sockfd])
    return -1;
  // reset timeout
  server->idle[sockfd] = 0;
  // send data
  Buffer.write(server->buffer_map[sockfd], data, len);
  if (Buffer.flush(server->buffer_map[sockfd], sockfd) < 0)
    return -1;
  return 0;
}
/** Writes data to the socket, moving the data's pointer directly to the buffer.

Once the data was written, `free` will be called to free the data's memory.

On error, returns -1 otherwise returns the number of bytes already sent.
*/
static ssize_t srv_write_move(struct Server* server,
                              int sockfd,
                              void* data,
                              size_t len) {
  // make sure the socket is alive
  if (!server->protocol_map[sockfd])
    return -1;
  // reset timeout
  server->idle[sockfd] = 0;
  // send data
  Buffer.write_move(server->buffer_map[sockfd], data, len);
  if (Buffer.flush(server->buffer_map[sockfd], sockfd) < 0)
    return -1;
  return 0;
}
/** Copies & writes data to the socket, managing an asyncronous buffer.

Each call to a `write` function considers it's data atomic (a single package).

The `urgent` varient will send the data as soon as possible, without distrupting
any data packages (data written using `write` will not be interrupted in the
middle).

On error, returns -1 otherwise returns the number of bytes already sent.
*/
static ssize_t srv_write_urgent(struct Server* server,
                                int sockfd,
                                void* data,
                                size_t len) {
  // make sure the socket is alive
  if (!server->protocol_map[sockfd])
    return -1;
  // reset timeout
  server->idle[sockfd] = 0;
  // send data
  Buffer.write_next(server->buffer_map[sockfd], data, len);
  if (Buffer.flush(server->buffer_map[sockfd], sockfd) < 0)
    return -1;
  return 0;
}
/** Writes data to the socket, moving the data's pointer directly to the buffer.

Once the data was written, `free` will be called to free the data's memory.

Each call to a `write` function considers it's data atomic (a single package).

The `urgent` varient will send the data as soon as possible, without distrupting
any data packages (data written using `write` will not be interrupted in the
middle).

On error, returns -1 otherwise returns the number of bytes already sent.
*/
static ssize_t srv_write_move_urgent(struct Server* server,
                                     int sockfd,
                                     void* data,
                                     size_t len) {
  // make sure the socket is alive
  if (!server->protocol_map[sockfd])
    return -1;
  // reset timeout
  server->idle[sockfd] = 0;
  // send data
  Buffer.write_move_next(server->buffer_map[sockfd], data, len);
  if (Buffer.flush(server->buffer_map[sockfd], sockfd) < 0)
    return -1;
  return 0;
}
/**
Sends a whole file as if it were a single atomic packet.

Once the file was sent, the `FILE *` will be closed using `fclose`.

The file will be buffered to the socket chunk by chunk, so that memory
consumption is capped at ~ 64Kb.
*/
static ssize_t srv_sendfile(struct Server* server, int sockfd, FILE* file) {
  // make sure the socket is alive
  if (!server->protocol_map[sockfd])
    return -1;
  // reset timeout
  server->idle[sockfd] = 0;
  // send data
  Buffer.sendfile(server->buffer_map[sockfd], file);
  if (Buffer.flush(server->buffer_map[sockfd], sockfd) < 0)
    return -1;
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Tasks + Async

// Global Task helpers
static inline int perform_single_task(server_pt srv,
                                      int fd,
                                      void (*task)(server_pt server,
                                                   int fd,
                                                   void* arg),
                                      void* arg) {
  if (set_to_busy(srv, fd)) {
    // perform the task
    task(srv, fd, arg);
    // release the busy flag
    srv->busy[fd] = 0;
    // return completion flag;
    return 1;
  }
  return 0;
}

// FDTask helpers
struct FDTask* new_fd_task(server_pt srv) {
  struct FDTask* ret = NULL;
  pthread_mutex_lock(&srv->task_lock);
  if (srv->fd_task_pool) {
    ret = srv->fd_task_pool;
    srv->fd_task_pool = srv->fd_task_pool->next;
    --srv->fd_task_pool_size;
    pthread_mutex_unlock(&srv->task_lock);
    return ret;
  }
  pthread_mutex_unlock(&srv->task_lock);
  ret = malloc(sizeof(*ret));
  return ret;
}
void destroy_fd_task(server_pt srv, struct FDTask* task) {
  if (task == NULL) {
    pthread_mutex_lock(&srv->task_lock);
    struct FDTask* tmp;
    srv->fd_task_pool_size = 0;
    while ((tmp = srv->fd_task_pool)) {
      srv->fd_task_pool = srv->fd_task_pool->next;
      free(tmp);
    }
    pthread_mutex_unlock(&srv->task_lock);
    return;
  }
  pthread_mutex_lock(&srv->task_lock);
  if (srv->fd_task_pool_size >= 128) {
    free(task);
    pthread_mutex_unlock(&srv->task_lock);
    return;
  }
  task->next = srv->fd_task_pool;
  srv->fd_task_pool = task;
  ++srv->fd_task_pool_size;
  pthread_mutex_unlock(&srv->task_lock);
}
static void perform_fd_task(struct FDTask* task) {
  // is it okay to perform the task?
  if (task->server->protocol_map[task->fd]) {
    if (perform_single_task(task->server, task->fd, task->task, task->arg)) {
      // free the memory
      destroy_fd_task(task->server, task);
    } else
      Async.run(task->server->async, (void (*)(void*))perform_fd_task, task);
  } else {
    if (task->fallback)  // check for fallback, call if requested
      task->fallback(task->server, task->fd, task->arg);
    // free the memory
    destroy_fd_task(task->server, task);
  }
}

// GroupTask helpers
struct GroupTask* new_group_task(server_pt srv) {
  struct GroupTask* ret = NULL;
  pthread_mutex_lock(&srv->task_lock);
  if (srv->fd_task_pool) {
    ret = srv->group_task_pool;
    srv->group_task_pool = srv->group_task_pool->next;
    --srv->group_task_pool_size;
    pthread_mutex_unlock(&srv->task_lock);
    return ret;
  }
  pthread_mutex_unlock(&srv->task_lock);
  ret = malloc(sizeof(*ret) + (srv_capacity() >> 3) + 1);
  memset(ret, 0, sizeof(*ret) + (srv_capacity() >> 3) + 1);
  return ret;
}
void destroy_group_task(server_pt srv, struct GroupTask* task) {
  if (task == NULL) {
    pthread_mutex_lock(&srv->task_lock);
    struct FDTask* tmp;
    srv->group_task_pool_size = 0;
    while ((tmp = srv->fd_task_pool)) {
      srv->fd_task_pool = srv->fd_task_pool->next;
      free(tmp);
    }
    pthread_mutex_unlock(&srv->task_lock);
    return;
  }
  pthread_mutex_lock(&srv->task_lock);
  if (srv->group_task_pool_size >= 64) {
    free(task);
    pthread_mutex_unlock(&srv->task_lock);
    return;
  }
  task->next = srv->group_task_pool;
  srv->group_task_pool = task;
  ++srv->group_task_pool_size;
  pthread_mutex_unlock(&srv->task_lock);
}

static void perform_group_task(struct GroupTask* task) {
  // the maximum number of bytes to review (each bit == 1 fd);
  long fds = (srv_capacity() >> 3) + 1;
  // flag for pending tasks
  int pending = 0;
  unsigned int bit;
  int fd_target;
  while (fds) {
    // the byte contains at least one bit marker for a task related fd
    if (task->fds[fds]) {
      bit = 0;
      while (bit < 8) {
        fd_target = bit + (fds << 3);
        // is it okay to perform the task?
        if (task->fds[fds] & (1 << bit)) {
          if (task->server->protocol_map[fd_target]) {
            if (perform_single_task(task->server, fd_target, task->task,
                                    task->arg)) {
              task->fds[fds] &= ~(1 << bit);
              goto rescedule;
            }
            pending = 1;
          } else  // closed connection, ignore it and clear it's flag.
            task->fds[fds] &= ~(1 << bit);
        }
        bit++;
      }
    }
    fds--;
  }
  if (pending)
    goto rescedule;
  // clear the task away...
  if (task->on_finished) {
    if (fd_task(task->server, task->fd_origin, task->on_finished, task->arg,
                task->on_finished))
      task->on_finished(task->server, task->fd_origin, task->arg);
  }
  destroy_group_task(task->server, task);
  return;
rescedule:
  Async.run(task->server->async, (void (*)(void*)) & perform_group_task, task);
  return;
}

void add_to_group_task(struct Server* server, int fd, void* arg) {
  struct GroupTask* task = arg;
  task->fds[fd >> 3] |= (1 << (fd & 7));
}

/** Schedules a specific task to run asyncronously for each connection.
a NULL service identifier == all connections (all protocols). */
static int each(struct Server* server,
                char* service,
                void (*task)(struct Server* server, int fd, void* arg),
                void* arg,
                void (*on_finish)(struct Server* server, int fd, void* arg)) {
  struct GroupTask* gtask = new_group_task(server);
  if (!gtask || !task)
    return -1;
  gtask->arg = arg;
  gtask->task = task;
  gtask->server = server;
  gtask->on_finished = on_finish;
  int ret = each_block(server, service, add_to_group_task, gtask);
  Async.run(server->async, (void (*)(void*)) & perform_group_task, gtask);
  return ret;
}
/** Schedules a specific task to run for each connection. The tasks will be
 * performed sequencially, in a blocking manner. The method will only return
 * once all the tasks were completed. A NULL service identifier == all
 * connections (all protocols).
*/
static int each_block(struct Server* server,
                      char* service,
                      void (*task)(struct Server* server, int fd, void* arg),
                      void* arg) {
  int c = 0;
  if (service) {
    for (int i = 0; i < server->capacity; i++) {
      if (server->protocol_map[i] && server->protocol_map[i]->service &&
          (server->protocol_map[i]->service == service ||
           !strcmp(server->protocol_map[i]->service, service))) {
        task(server, i, arg);
        ++c;
      }
    }
  } else {
    for (int i = 0; i < server->capacity; i++) {
      if (server->protocol_map[i] &&
          server->protocol_map[i]->service != timer_protocol_name) {
        task(server, i, arg);
        ++c;
      }
    }
  }
  return c;
}
/** Schedules a specific task to run asyncronously for a specific connection.

returns -1 on failure, 0 on success (success being scheduling or performing
the task).
*/
static int fd_task(struct Server* server,
                   int sockfd,
                   void (*task)(struct Server* server, int fd, void* arg),
                   void* arg,
                   void (*fallback)(struct Server* server, int fd, void* arg)) {
  if (server->protocol_map[sockfd]) {
    struct FDTask* msg = new_fd_task(server);
    if (!msg)
      return -1;
    *msg = (struct FDTask){
        .server = server, .task = task, .arg = arg, .fallback = fallback};
    Async.run(server->async, (void (*)(void*)) & perform_fd_task, msg);
    return 0;
  }
  return -1;
}
/** Runs an asynchronous task, IF threading is enabled (set the
`threads` to 1 (the default) or greater).

If threading is disabled, the current thread will perform the task and return.

Returns -1 on error or 0
on succeess.
*/
static int run_async(struct Server* self, void task(void*), void* arg) {
  return Async.run(self->async, task, arg);
}
/** Creates a system timer (at the cost of 1 file descriptor) and pushes the
timer to the reactor. The task will NOT repeat. Returns -1 on error or the
new file descriptor on succeess.

NOTICE: Do NOT create timers from within the on_close callback, as this might
block resources from being properly freed (if the timer and the on_close
object share the same fd number).
*/
static int run_after(struct Server* self,
                     long milliseconds,
                     void task(void*),
                     void* arg) {
  return run_every(self, milliseconds, 1, task, arg);
}
/** Creates a system timer (at the cost of 1 file descriptor) and pushes the
timer to the reactor. The task will repeat `repetitions` times. if
`repetitions` is set to 0, task will repeat forever. Returns -1 on error
or the new file descriptor on succeess.

NOTICE: Do NOT create timers from within the on_close callback, as this might
block resources from being properly freed (if the timer and the on_close
object share the same fd number).
*/
static int run_every(struct Server* self,
                     long milliseconds,
                     int repetitions,
                     void task(void*),
                     void* arg) {
  int tfd = reactor_make_timer();
  if (tfd <= 0)
    return -1;
  struct Protocol* timer_protocol =
      (struct Protocol*)TimerProtocol(task, arg, repetitions);
  if (srv_attach(self, tfd, timer_protocol)) {
    free(timer_protocol);
    return -1;
  }
  // remove the default timeout (timers shouldn't timeout)
  self->tout[tfd] = 0;

  // srv_attach connected the fd as a regular socket - remove it and reconnect
  // as a timer
  reactor_remove((struct Reactor*)self, tfd);
  if (reactor_add_timer((struct Reactor*)self, tfd, milliseconds) < 0) {
    perror("Closing timer");
    printf("Timer %d closing for server with sock %d, epoll %d, map data: %d\n",
           tfd, self->srvfd, self->reactor.private.reactor_fd,
           self->reactor.private.map[tfd]);
    // close(tfd);
    // on_close might be called by the server to free the resources - we
    // shouldn't race... but we are making some changes...
    reactor_close((struct Reactor*)self, tfd);
    return -1;
  };
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Server Registry
// (stopping and signal managment)

// types and global data
struct ServerSet {
  struct ServerSet* next;
  struct Server* server;
};
static struct ServerSet* global_servers_set = NULL;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

// register a server
static void register_server(server_pt srv) {
  pthread_mutex_lock(&global_lock);
  struct ServerSet* n_reg = malloc(sizeof(struct ServerSet));
  n_reg->server = srv;
  n_reg->next = global_servers_set;
  global_servers_set = n_reg;
  pthread_mutex_unlock(&global_lock);
}
// stop a server (+unregister)
static void srv_stop(server_pt srv) {
  pthread_mutex_lock(&global_lock);
  // remove from regisrty
  struct ServerSet* set = global_servers_set;
  struct ServerSet* tmp = global_servers_set;
  if (global_servers_set && global_servers_set->server == srv) {
    global_servers_set = global_servers_set->next;
    free(tmp);
    goto sig_srv;
  } else
    while (set) {
      if (set->next && set->next->server == srv) {
        tmp = set->next;
        set->next = set->next->next;
        free(tmp);
        goto sig_srv;
      }
      set = set->next;
    }
  // the server wasn't in the registry - we shouldn't stop it again...
  srv = NULL;
// send a signal to the server, if it was in the registry
sig_srv:
  if (srv) {
    // close the listening socket, preventing new connections from coming in.
    reactor_add((struct Reactor*)srv, srv->srvfd);
    // set the stop server flag.
    srv->run = 0;
    // signal the async object to finish
    Async.signal(srv->async);
  }
  pthread_mutex_unlock(&global_lock);
}
// deregisters and stops all servers
static void srv_stop_all(void) {
  while (global_servers_set)
    srv_stop(global_servers_set->server);
}

// handles signals
static void on_signal(int sig) {
  if (!global_servers_set) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  srv_stop_all();
}

////////////////////////////////////////////////////////////////////////////////
// socket helpers

// sets a socket to non blocking state
static inline int set_non_blocking_socket(int fd)  // Thanks to Bjorn Reese
{
/* If they have O_NONBLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
  /* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
  static int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
    flags = 0;
  // printf("flags initial value was %d\n", flags);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  /* Otherwise, use the old way of doing it */
  static int flags = 1;
  return ioctl(fd, FIOBIO, &flags);
#endif
}

// helper functions used in the context of this file
static int bind_server_socket(struct Server* self) {
  int srvfd;
  // setup the address
  struct addrinfo hints;
  struct addrinfo* servinfo;        // will point to the results
  memset(&hints, 0, sizeof hints);  // make sure the struct is empty
  hints.ai_family = AF_UNSPEC;      // don't care IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets
  hints.ai_flags = AI_PASSIVE;      // fill in my IP for me
  if (getaddrinfo(self->settings->address, self->settings->port, &hints,
                  &servinfo)) {
    perror("addr err");
    return -1;
  }
  // get the file descriptor
  srvfd =
      socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
  if (srvfd <= 0) {
    perror("socket err");
    freeaddrinfo(servinfo);
    return -1;
  }
  // make sure the socket is non-blocking
  if (set_non_blocking_socket(srvfd) < 0) {
    perror("couldn't set socket as non blocking! ");
    freeaddrinfo(servinfo);
    close(srvfd);
    return -1;
  }
  // avoid the "address taken"
  {
    int optval = 1;
    setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
  }
  // bind the address to the socket
  {
    int bound = 0;
    for (struct addrinfo* p = servinfo; p != NULL; p = p->ai_next) {
      if (!bind(srvfd, p->ai_addr, p->ai_addrlen))
        bound = 1;
    }

    if (!bound) {
      perror("bind err");
      freeaddrinfo(servinfo);
      close(srvfd);
      return -1;
    }
  }
  freeaddrinfo(servinfo);
  // listen in
  if (listen(srvfd, SOMAXCONN) < 0) {
    perror("couldn't start listening");
    close(srvfd);
    return -1;
  }
  return srvfd;
}

////////////////
// file limit helper
static long srv_capacity(void) {
  // get current limits
  static long flim = 0;
  if (flim)
    return flim;
#ifdef _SC_OPEN_MAX
  flim = sysconf(_SC_OPEN_MAX) - 1;
#elif defined(OPEN_MAX)
  flim = OPEN_MAX - 1;
#endif
  // try to maximize limits - collect max and set to max
  struct rlimit rlim;
  getrlimit(RLIMIT_NOFILE, &rlim);
  // printf("Meximum open files are %llu out of %llu\n", rlim.rlim_cur,
  //        rlim.rlim_max);
  rlim.rlim_cur = rlim.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rlim);
  getrlimit(RLIMIT_NOFILE, &rlim);
  // printf("Meximum open files are %llu out of %llu\n", rlim.rlim_cur,
  //        rlim.rlim_max);
  // if the current limit is higher than it was, update
  if (flim < rlim.rlim_cur)
    flim = rlim.rlim_cur;
  // review stack memory limits - don't use more than half...?
  // use 1 byte (char) for busy_map;
  // use 1 byte (char) for idle_map;
  // use 1 byte (char) for tout_map;
  // use 8 byte (void *) for protocol_map;
  // use 8 byte (void *) for server_map;
  // ------
  // total of 19 (assume 20) bytes per connection.
  //
  // assume a 2Kb stack allocation (per connection) for
  // network IO buffer and request management...?
  // (this would be almost wrong to assume, as buffer might be larger)
  // -------
  // 2068 Byte
  // round up to page size?
  // 4096
  //
  getrlimit(RLIMIT_STACK, &rlim);
  if (flim * 4096 > rlim.rlim_cur && flim * 4096 < rlim.rlim_max) {
    rlim.rlim_cur = flim * 4096;
    setrlimit(RLIMIT_STACK, &rlim);
    getrlimit(RLIMIT_STACK, &rlim);
  }
  if (flim > rlim.rlim_cur / 4096) {
    // printf("The current maximum file limit (%d) if reduced due to stack
    // memory "
    //        "limits(%d)\n",
    //        flim, (int)(rlim.rlim_cur / 2068));
    flim = rlim.rlim_cur / 4096;
  } else {
    // printf(
    //     "The current maximum file limit (%d) is supported by the stack
    //     memory
    //     "
    //     "limits(%d)\n",
    //     flim, (int)(rlim.rlim_cur / 2068));
  }

  // how many Kb per connection? assume 8Kb for kernel? x2 (16Kb).
  // (http://www.metabrew.com/article/a-million-user-comet-application-with-mochiweb-part-3)
  // 10,000 connections == 16*1024*10000 == +- 160Mb? seems a tight fit...
  // i.e. the Http request buffer is 8Kb... maybe 24Kb is a better minimum?
  // Some per connection heap allocated data (i.e. 88 bytes per user-land
  // buffer) also matters.
  getrlimit(RLIMIT_DATA, &rlim);
  if (flim > rlim.rlim_cur / (24 * 1024)) {
    printf(
        "The current maximum file limit (%ld) if reduced due to system "
        "memory "
        "limits(%ld)\n",
        flim, (long)(rlim.rlim_cur / (24 * 1024)));
    flim = rlim.rlim_cur / (24 * 1024);
  } else {
    // printf(
    //     "The current maximum file limit (%d) is supported by the stack
    //     memory
    //     "
    //     "limits(%d)\n",
    //     flim, (int)(rlim.rlim_cur / 2068));
  }
  // return what we have
  return flim;
}
