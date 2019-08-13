//
// Created by Keren Dong on 2019-08-10.
//

#ifndef KUNGFU_WINGCHUN_COMMANDER_H
#define KUNGFU_WINGCHUN_COMMANDER_H

#include <kungfu/yijinjing/log/setup.h>
#include <kungfu/yijinjing/io.h>
#include <kungfu/practice/apprentice.h>

namespace kungfu
{
    namespace wingchun
    {
        class Commander : public practice::apprentice
        {
        public:
            explicit Commander(yijinjing::data::locator_ptr locator, bool low_latency = false, const std::string &name = "commander");

            virtual ~Commander() = default;

            virtual std::string handle_request(const std::string &msg) = 0;

        protected:

            void register_location(int64_t trigger_time, const yijinjing::data::location_ptr &location) override;

            void deregister_location(int64_t trigger_time, uint32_t location_uid) override;

            void on_write_to(const yijinjing::event_ptr &event) override;

            void on_read_from(const yijinjing::event_ptr &event) override;

            void on_start() override;

        private:

            void connect(int64_t trigger_time, const yijinjing::data::location_ptr &app_location);

        };
    }
}
#endif //KUNGFU_WINGCHUN_COMMANDER_H