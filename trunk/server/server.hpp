// http://code.google.com/p/server1/
//
// You can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Author: xiliu.tang@gmail.com (Xiliu Tang)

#ifndef NET2_SERVER_HPP
#define NET2_SERVER_HPP

#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include "server/connection.hpp"
#include "server/io_service_pool.hpp"
#include "thread/threadpool.hpp"

class AcceptorHandler;
// The top-level class of the Server.
class Server
  : private boost::noncopyable {
public:
  /// Construct the Server to listen on the specified TCP address and port, and
  /// serve up files from the given directory.
  explicit Server(int io_service_number,
                  int worker_threads);
  ~Server();

  void Listen(const string &address, const string &port,
              Connection* connection_template);

  /// Stop the Server.
  void Stop();
private:
  struct AcceptorResource {
    boost::asio::ip::tcp::acceptor *acceptor;
    boost::asio::ip::tcp::socket **socket_pptr;
    AcceptorResource(boost::asio::ip::tcp::acceptor *in_acceptor,
                     boost::asio::ip::tcp::socket **in_socket_pptr)
      : acceptor(in_acceptor), socket_pptr(in_socket_pptr) {
    }
    void Release() {
      delete acceptor;
      delete *socket_pptr;
      delete socket_pptr;
    }
  };
  typedef hash_map<string, AcceptorResource> AcceptorTable;
  void ReleaseAcceptor(const string &host);

  void RemoveConnection(Connection *connection);
  typedef hash_set<Connection*> ConnectionTable;
  // Handle completion of an asynchronous accept operation.
  void HandleAccept(const boost::system::error_code& e,
                    Connection *new_connection);

  // The pool of io_service objects used to perform asynchronous operations.
  IOServicePool io_service_pool_;
  ThreadPool threadpool_;
  friend class AcceptorHandler;
  ConnectionTable connection_table_;
  boost::mutex connection_table_mutex_;

  AcceptorTable acceptor_table_;
  boost::mutex acceptor_table_mutex_;
  bool is_running_;
};
#endif // NET2_SERVER_HPP