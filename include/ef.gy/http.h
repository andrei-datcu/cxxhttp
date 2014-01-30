/**\file
 * \brief Boost::ASIO HTTP Server
 *
 * An asynchornous HTTP server implementation using Boost::ASIO and
 * Boost::Regex. You will need to have Boost installed and available when
 * using this header.
 *
 * \copyright
 * Copyright (c) 2012-2014, ef.gy Project Members
 * \copyright
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * \copyright
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * \copyright
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * \see Project Documentation: http://ef.gy/documentation/libefgy
 * \see Project Source Code: http://git.becquerel.org/jyujin/libefgy.git
 */

#if !defined(EF_GY_HTTP_H)
#define EF_GY_HTTP_H

#include <map>
#include <string>
#include <sstream>

#include <iostream>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/regex.hpp>

#include <boost/algorithm/string.hpp>

namespace efgy
{
    /**\brief Networking code
     *
     * Contains templates that deal with networking in one way or another.
     * Currently only contains an HTTP server.
     */
    namespace net
    {
        /**\brief HTTP handling
         *
         * Contains an HTTP server and templates for session management and
         * processing by user code.
         */
        namespace http
        {
            /**\brief HTTP state classes
             *
             * Contains state classes which are used to keep track of the data
             * that a request processor functor needs to respond to a client's
             * HTTP request.
             */
            namespace state
            {
                /**\brief Stub state class
                 *
                 * This is the default state class, which contains no data at
                 * all. Many kinds of requests do not need additional data to
                 * process requests, and they'll be perfectly fine with this
                 * state class.
                 */
                class stub
                {
                    public:
                        /**\brief Constructor
                         *
                         * Discards the argument passed to it and does nothing.
                         */
                        stub(void *) {}
                };
            };

            template<typename requestProcessor, typename stateClass = state::stub>
            class server;

            /**\brief Session wrapper
             *
             * Used by the server to keep track of all the data associated with
             * a single, asynchronous client connection.
             *
             * \tparam requestProcessor The functor class to handle requests.
             * \tparam stateClass       Data class needed to keep track of the
             *                          resources used by the requestProcessor.
             */
            template<typename requestProcessor, typename stateClass = state::stub>
            class session
            {
                protected:
                    static const int maxContentLength = (1024 * 1024 * 12);

                    enum
                    {
                        stRequest,
                        stHeader,
                        stContent,
                        stProcessing,
                        stErrorContentTooLarge,
                        stShutdown
                    } status;

                    class caseInsensitiveLT : private std::binary_function<std::string, std::string, bool>
                    {
                        public:
                            bool operator() (const std::string &a, const std::string &b) const
                            {
                                return lexicographical_compare (a, b, boost::is_iless());
                            }

                    };

                public:
                    boost::asio::local::stream_protocol::socket socket;
                    std::string method;
                    std::string resource;
                    std::map<std::string,std::string,caseInsensitiveLT> header;
                    std::string content;
                    stateClass *state;

                    session (boost::asio::io_service &pIOService)
                        : socket(pIOService), status(stRequest), input()
                        {}

                    ~session (void)
                        {
                            status = stShutdown;
                            socket.shutdown(boost::asio::local::stream_protocol::socket::shutdown_both);
                            socket.cancel();
                            socket.close();
                        }

                    void start (stateClass *state)
                    {
                        this->state = state;
                        read();
                    }

                    void reply (int status, const std::string &header, const std::string &body)
                    {
                        std::stringstream reply;
                        reply << "HTTP/1.1 " << status << " NA\r\nContent-Length: " << body.length() << "\r\n" + header + "\r\n" + body;

                        if (status < 400)
                        {
                            boost::asio::async_write
                                (socket,
                                 boost::asio::buffer(reply.str()),
                                 boost::bind (&session::handleWrite, this,
                                              boost::asio::placeholders::error));
                        }
                        else
                        {
                            boost::asio::async_write
                                (socket,
                                 boost::asio::buffer(reply.str()),
                                 boost::bind (&session::handleWriteClose, this,
                                              boost::asio::placeholders::error));
                        }
                    }

                    void reply (int status, const std::string &body)
                    {
                        reply (status, "", body);
                    }

                protected:
                    void handleRead(const boost::system::error_code &error, size_t bytes_transferred)
                    {
                        if (status == stShutdown)
                        {
                            return;
                        }

                        if (!error)
                        {
                            static const boost::regex req("(\\w+)\\s+([\\w\\d%/.:;()+-]+)\\s+HTTP/1.[01]\\s*");
                            static const boost::regex mime("([\\w-]+):\\s*(.*)\\s*");
                            static const boost::regex mimeContinued("[ \t]\\s*(.*)\\s*");

                            std::istream is(&input);
                            std::string s;

                            boost::smatch matches;

                            switch(status)
                            {
                                case stRequest:
                                case stHeader:
                                    std::getline(is,s);
                                    break;
                                case stContent:
                                    s = std::string(contentLength, '\0');
                                    is.read(&s[0], contentLength);
                                    break;
                                case stProcessing:
                                case stErrorContentTooLarge:
                                case stShutdown:
                                    break;
                            }

                            switch(status)
                            {
                                case stRequest:
                                    if (boost::regex_match(s, matches, req))
                                    {
                                        method   = matches[1];
                                        resource = matches[2];

                                        header = std::map<std::string,std::string,caseInsensitiveLT>();
                                        status = stHeader;
                                    }
                                    break;

                                case stHeader:
                                    if ((s == "\r") || (s == ""))
                                    {
                                        try
                                        {
                                            contentLength = std::atoi(std::string(header["Content-Length"]).c_str());
                                        }
                                        catch(...)
                                        {
                                            contentLength = 0;
                                        }

                                        if (contentLength > maxContentLength)
                                        {
                                            status = stErrorContentTooLarge;
                                            reply (400, "Request body too large");
                                        }
                                        else
                                        {
                                            status = stContent;
                                        }
                                    }
                                    else if (boost::regex_match(s, matches, mimeContinued))
                                    {
                                        header[lastHeader] += "," + matches[1];
                                    }
                                    else if (boost::regex_match(s, matches, mime))
                                    {
                                        lastHeader = matches[1];
                                        header[matches[1]] = matches[2];
                                    }

                                    break;

                                case stContent:
                                    content = s;
                                    status = stProcessing;

                                    /* processing the request takes places here */
                                    {
                                        requestProcessor rp;
                                        rp(*this);
                                    }

                                    break;

                                case stProcessing:
                                case stErrorContentTooLarge:
                                case stShutdown:
                                    break;
                            }

                            switch(status)
                            {
                                case stRequest:
                                case stHeader:
                                case stContent:
                                    read();
                                case stProcessing:
                                case stErrorContentTooLarge:
                                case stShutdown:
                                    break;
                            }
                        }
                        else
                        {
                            delete this;
                        }
                    }

                    void handleWrite(const boost::system::error_code &error)
                    {
                        if (status == stShutdown)
                        {
                            return;
                        }

                        if (!error)
                        {
                            if (status == stProcessing)
                            {
                                status = stRequest;
                                read();
                            }
                        }
                        else
                        {
                            delete this;
                        }
                    }

                    void handleWriteClose(const boost::system::error_code &error)
                    {
                        if (status == stShutdown)
                        {
                            return;
                        }

                        delete this;
                    }

                    void read(void)
                    {
                        switch (status)
                        {
                            case stRequest:
                            case stHeader:
                                boost::asio::async_read_until
                                    (socket, input, "\n", 
                                     boost::bind(&session::handleRead, this,
                                                 boost::asio::placeholders::error,
                                                 boost::asio::placeholders::bytes_transferred));
                                break;
                            case stContent:
                                boost::asio::async_read
                                    (socket, input,
                                     boost::bind(&session::contentReadP, this,
                                                 boost::asio::placeholders::error,
                                                 boost::asio::placeholders::bytes_transferred),
                                     boost::bind(&session::handleRead, this,
                                                 boost::asio::placeholders::error,
                                                 boost::asio::placeholders::bytes_transferred));
                                break;
                            case stProcessing:
                            case stErrorContentTooLarge:
                            case stShutdown:
                                break;
                        }
                    }

                    std::size_t contentReadP (const boost::system::error_code& error, std::size_t bytes_transferred)
                    {
                        return (bool(error) || (bytes_transferred >= contentLength)) ? 0 : (contentLength - bytes_transferred);
                    }

                    std::string lastHeader;
                    std::size_t contentLength;

                    boost::asio::streambuf input;
            };

            /**\brief HTTP server wrapper
             *
             * Contains the code that accepts incoming HTTP requests and
             * dispatches asynchronous processing.
             *
             * \tparam requestProcessor The functor class to handle requests.
             * \tparam stateClass       Data class needed to keep track of the
             *                          resources used by the requestProcessor.
             */
            template<typename requestProcessor, typename stateClass>
            class server
            {
                public:
                    /**\brief Initialise with IO service
                     *
                     * Default constructor which binds an IOService to a UNIX
                     * socket. The socket is bound asynchronously.
                     *
                     * \param[out] pIOService IO service to use.
                     * \param[in]  socket     UNIX socket to bind.
                     * \param[in]  stateData  Data to pas to the state class.
                     */
                    server(boost::asio::io_service &pIOService, const char *socket, void *stateData = 0)
                        : IOService(pIOService),
                          acceptor(pIOService, boost::asio::local::stream_protocol::endpoint(socket)),
                          state(stateData)
                        {
                            startAccept();
                        }

                protected:
                    void startAccept()
                    {
                        session<requestProcessor,stateClass> *new_session
                            = new session<requestProcessor,stateClass>(IOService);
                        acceptor.async_accept(new_session->socket,
                            boost::bind(&server::handleAccept, this, new_session,
                                        boost::asio::placeholders::error));
                    }

                    void handleAccept(session<requestProcessor,stateClass> *new_session, const boost::system::error_code &error)
                    {
                        if (!error)
                        {
                            new_session->start(&state);
                        }
                        else
                        {
                            delete new_session;
                        }

                        startAccept();
                    }

                    boost::asio::io_service &IOService;
                    boost::asio::local::stream_protocol::acceptor acceptor;
                    stateClass state;
            };
        };
    };
};

#endif
