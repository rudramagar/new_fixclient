#ifndef FIX_REGRESSION_H
#define FIX_REGRESSION_H

#include "socket.h"
#include "fix_message.h"
#include "fix_parser.h"
#include "fix_session_state.h"

#include <string>
#include <stdint.h>

bool run_fix_regression(TcpSocket& socket,
                        FixParser& fix_parser,
                        FixMessage& fix,
                        const std::string& scenarios_path,
                        SequenceState& sequence_state,
                        SentMessageStore& sent_messages,
                        uint64_t& last_send_ms,
                        bool& logon_accepted,
                        bool& scenarios_sent,
                        bool& scenario_response_started,
                        uint64_t& last_scenario_response_ms,
                        bool& logout_initiated);

#endif
