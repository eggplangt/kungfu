/*****************************************************************************
 * Copyright [taurus.ai]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include <typeinfo>
#include <spdlog/spdlog.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <nanomsg/pubsub.h>
#include <nanomsg/pipeline.h>
#include <nlohmann/json.hpp>

#include <kungfu/yijinjing/util/util.h>
#include <kungfu/yijinjing/util/os.h>
#include <kungfu/yijinjing/io.h>

using namespace kungfu::yijinjing;
using namespace kungfu::yijinjing::data;
using namespace kungfu::yijinjing::journal;
using namespace kungfu::yijinjing::nanomsg;

namespace kungfu
{
    namespace yijinjing
    {
        class ipc_url_factory : public url_factory
        {
        public:

            const std::string make_url_bind(const data::location_ptr location, protocol p) const override
            {
                auto locator = location->locator;
                return "ipc://" + locator->make_path(locator->socket_path(location), location->name + "." + get_protocol_name(p));
            }

            const std::string make_url_connect(const data::location_ptr location, protocol p) const override
            {
                auto locator = location->locator;
                return "ipc://" + locator->make_path(locator->socket_path(location), location->name + "." + get_protocol_name(get_opposite_protol(p)));
            }
        };

        class nanomsg_publisher : public publisher
        {
        public:
            nanomsg_publisher(bool low_latency, protocol p) : socket_(p), low_latency_(low_latency)
            {}

            ~nanomsg_publisher() override
            {
                SPDLOG_DEBUG("publisher closing");
                socket_.close();
                SPDLOG_DEBUG("publisher closed");
            }

            void init(const io_device &io)
            {
                auto location = std::make_shared<data::location>(data::mode::LIVE, data::category::SYSTEM, "master", "master", io.get_home()->locator);
                init_socket(socket_, location, io.get_url_factory());
            }

            int notify() override
            {
                return low_latency_ ? 0 : publish("{}");
            }

            int publish(const std::string &json_message) override
            {
                return socket_.send(json_message);
            }

        protected:
            virtual void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) = 0;

        private:
            const bool low_latency_;
            socket socket_;
        };

        class nanomsg_publisher_master : public nanomsg_publisher
        {
        public:
            nanomsg_publisher_master(bool low_latency) : nanomsg_publisher(low_latency, protocol::PUBLISH) {}

        protected:
            void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) override
            {
                s.bind(url_factory->make_url_bind(location, s.get_protocol()));
            }
        };

        class nanomsg_publisher_client : public nanomsg_publisher
        {
        public:
            nanomsg_publisher_client(bool low_latency) : nanomsg_publisher(low_latency, protocol::PUSH) {}

        protected:
            void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) override
            {
                s.connect(url_factory->make_url_connect(location, s.get_protocol()));
            }
        };

        class nanomsg_observer : public observer
        {
        public:
            nanomsg_observer(bool low_latency, protocol p) : low_latency_(low_latency), socket_(p)
            {}

            void init(const io_device &io)
            {
                int timeout = low_latency_ ? 0 : DEFAULT_NOTICE_TIMEOUT;
                auto location = std::make_shared<data::location>(data::mode::LIVE, data::category::SYSTEM, "master", "master", io.get_home()->locator);
                init_socket(socket_, location, io.get_url_factory());
                socket_.setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
                SPDLOG_DEBUG("observing master chanel with timeout {}ms [{}]", timeout, socket_.get_url());
            }

            virtual ~nanomsg_observer()
            {
                SPDLOG_DEBUG("observer closing");
                socket_.close();
                SPDLOG_DEBUG("observer closed");
            }

            bool wait() override
            {
                return socket_.recv() > 0;
            }

            const std::string &get_notice() override
            {
                return socket_.last_message();
            }

        protected:

            virtual void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) = 0;

        private:
            const bool low_latency_;
            socket socket_;
        };

        class nanomsg_observer_master : public nanomsg_observer
        {
        public:
            nanomsg_observer_master(bool low_latency) : nanomsg_observer(low_latency, protocol::PULL) {}

        protected:
            void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) override 
            {
                s.bind(url_factory->make_url_bind(location, s.get_protocol()));
            }
        };

        class nanomsg_observer_client : public nanomsg_observer
        {
        public:
            nanomsg_observer_client(bool low_latency) : nanomsg_observer(low_latency, protocol::SUBSCRIBE) {}

        protected:
            void init_socket(socket & s, location_ptr location, url_factory_ptr url_factory) override
            {
                s.connect(url_factory->make_url_connect(location, s.get_protocol()));
                s.setsockopt_str(NN_SUB, NN_SUB_SUBSCRIBE, "");
            }
        };

        io_device::io_device(data::location_ptr home, const bool low_latency, const bool lazy) : home_(home), low_latency_(low_latency), lazy_(lazy)
        {
            url_factory_ = std::make_shared<ipc_url_factory>();
        }

        reader_ptr io_device::open_reader_to_subscribe()
        {
            return std::make_shared<reader>(lazy_);
        }

        reader_ptr io_device::open_reader(const data::location_ptr location, uint32_t dest_id)
        {
            auto r = std::make_shared<reader>(lazy_);
            r->subscribe(location, dest_id, 0);
            return r;
        }

        writer_ptr io_device::open_writer(uint32_t dest_id)
        {
            return std::make_shared<writer>(home_, dest_id, lazy_, publisher_);
        }

        socket_ptr io_device::connect_socket(const data::location_ptr location, const protocol &p, int timeout)
        {
            socket_ptr s = std::make_shared<socket>(p);
            auto url = url_factory_->make_url_connect(location, p);
            s->connect(url);
            s->setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
            SPDLOG_INFO("connected socket [{}] {} at {} with timeout {}", nanomsg::get_protocol_name(p), location->name, url, timeout);
            return s;
        }

        socket_ptr io_device::bind_socket(const data::location_ptr location, const protocol &p, int timeout)
        {
            socket_ptr s = std::make_shared<socket>(p);
            auto url = url_factory_->make_url_bind(location, p);
            s->bind(url);
            s->setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
            SPDLOG_INFO("bind to socket [{}] {} at {} with timeout {}", nanomsg::get_protocol_name(p), location->name, url, timeout);
            return s;
        }

        io_device_master::io_device_master(data::location_ptr home, bool low_latency) : io_device(home, low_latency, false)
        {
            SPDLOG_DEBUG("creating io_device_master {}", home->uname);
            auto publisher = std::make_shared<nanomsg_publisher_master>(low_latency);
            publisher->init(*this);
            publisher_ = publisher;
            auto observer = std::make_shared<nanomsg_observer_master>(low_latency);
            observer->init(*this);
            observer_ = observer;
            service_socket_ = bind_socket(home, protocol::REPLY);
        }

        io_device_client::io_device_client(data::location_ptr home, bool low_latency) : io_device(home, low_latency, true)
        {
            SPDLOG_DEBUG("creating io_device_client {}", home->uname);
            auto publisher = std::make_shared<nanomsg_publisher_client>(low_latency);
            publisher->init(*this);
            publisher_ = publisher;
            auto observer = std::make_shared<nanomsg_observer_client>(low_latency);
            observer->init(*this);
            observer_ = observer;
            request_socket_ = connect_socket(home, protocol::REQUEST);
        }
    }
}


