#include "application.h"
#include "config_parser.h"
#include "fix_parser.h"
#include "fix_message.h"
#include "fix_template.h"
#include "token_handler.h"
#include "utils.h"
#include "fix_regression.h"
#include "fix_session_state.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <cctype>

const int peer_closed = 0;
const size_t receive_buffer_size = 4096;
const int logon_timeout_seconds = 5;
const int receive_timeout_millis = 200;
static bool is_running_regression = false;

static bool set_socket_recv_timeout(int sock_fd, int timeout_millis) {
    timeval tv;
    tv.tv_sec = timeout_millis / 1000;
    tv.tv_usec = (timeout_millis % 1000) * 1000;

    const int rc = ::setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return rc == 0;
}

static bool recv_timed_out() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static bool send_fix_message(TcpSocket& socket,
                             const std::string& message,
                             uint64_t& last_send_ms) {
    if (message.empty()) {
        return false;
    }

    if (!is_running_regression) {
        std::printf(">> %s\n", utils::to_pipe_delimited(message).c_str());
    }

    if (!socket.send_bytes(message)) {
        return false;
    }

    last_send_ms = utils::get_monotonic_millis();
    return true;
}

static bool get_int_tag(const std::string& fix_message,
                        const char* tag_prefix,
                        int& value) {
    std::string text;
    if (!utils::find_tag_value(fix_message, tag_prefix, text)) {
        return false;
    }

    char* end_ptr = 0;
    const long number = std::strtol(text.c_str(), &end_ptr, 10);

    if (end_ptr == text.c_str() || number <= 0 || number > 2000000000L) {
        return false;
    }

    value = static_cast<int>(number);
    return true;
}

static bool is_admin_message(const std::string& msg_type) {
    return msg_type == "0" ||
           msg_type == "1" ||
           msg_type == "2" ||
           msg_type == "3" ||
           msg_type == "4" ||
           msg_type == "5" ||
           msg_type == "A";
}

static std::string resend_store_path;

static char to_hex_char(unsigned int value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

static int from_hex_char(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static std::string hex_encode(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size() * 2);

    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        encoded.push_back(to_hex_char((byte >> 4) & 0x0F));
        encoded.push_back(to_hex_char(byte & 0x0F));
    }

    return encoded;
}

static bool hex_decode(const std::string& value, std::string& decoded) {
    if ((value.size() % 2) != 0) {
        return false;
    }

    decoded.clear();
    decoded.reserve(value.size() / 2);

    for (size_t i = 0; i < value.size(); i += 2) {
        const int high = from_hex_char(value[i]);
        const int low = from_hex_char(value[i + 1]);

        if (high < 0 || low < 0) {
            return false;
        }

        decoded.push_back(static_cast<char>((high << 4) | low));
    }

    return true;
}

static std::string get_resend_store_path(const std::string& token_path) {
    const std::string suffix = ".token";

    if (token_path.size() >= suffix.size() &&
        token_path.substr(token_path.size() - suffix.size()) == suffix) {
        return token_path.substr(0, token_path.size() - suffix.size()) + ".resend";
    }

    return token_path + ".resend";
}

static bool append_resend_store_message(const std::string& path,
                                        int msg_seq_num,
                                        const std::string& message) {
    if (path.empty()) {
        return true;
    }

    std::ofstream out(path.c_str(), std::ios::out | std::ios::app);
    if (!out) {
        std::printf("Warning: could not open resend store for append: %s\n", path.c_str());
        return false;
    }

    out << msg_seq_num << "|" << hex_encode(message) << "\n";
    return true;
}

static bool load_resend_store(const std::string& path,
                              SentMessageStore& sent_messages) {
    if (path.empty()) {
        return true;
    }

    std::ifstream in(path.c_str());
    if (!in) {
        return true;
    }

    std::string line;
    int loaded_count = 0;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        const size_t sep = line.find('|');
        if (sep == std::string::npos) {
            continue;
        }

        const int seq = std::atoi(line.substr(0, sep).c_str());
        if (seq <= 0) {
            continue;
        }

        std::string message;
        if (!hex_decode(line.substr(sep + 1), message)) {
            continue;
        }

        sent_messages[seq] = message;
        loaded_count++;
    }

    if (loaded_count > 0) {
        std::printf("Info: Loaded %d messages from resend store %s\n",
                    loaded_count,
                    path.c_str());
    }

    return true;
}

static void store_sent_message(SentMessageStore& sent_messages,
                               const std::string& message) {
    int msg_seq_num = 0;
    if (!get_int_tag(message, "34=", msg_seq_num)) {
        return;
    }

    sent_messages[msg_seq_num] = message;
    append_resend_store_message(resend_store_path, msg_seq_num, message);
}

static bool send_current_message(TcpSocket& socket,
                                 const std::string& message,
                                 uint64_t& last_send_ms,
                                 SequenceState& sequence_state,
                                 SentMessageStore& sent_messages) {
    if (!send_fix_message(socket, message, last_send_ms)) {
        return false;
    }

    store_sent_message(sent_messages, message);
    sequence_state.outbound_seq++;
    save_sequence_token(sequence_state);
    return true;
}

static void parse_fix_fields(const std::string& fix_message,
                             FixMessage::FieldList& fields) {
    fields.clear();
    fields.reserve(64);

    size_t pos = 0;
    while (pos < fix_message.size()) {
        size_t end = fix_message.find('\x01', pos);
        if (end == std::string::npos) {
            break;
        }

        const std::string field = fix_message.substr(pos, end - pos);
        pos = end + 1;

        const size_t eq = field.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }

        const int tag = std::atoi(field.substr(0, eq).c_str());
        if (tag <= 0) {
            continue;
        }

        fields.push_back(FixMessage::Field(tag, field.substr(eq + 1)));
    }
}

static std::string build_poss_dup_message(FixMessage& fix,
                                          const std::string& original_message) {
    FixMessage::FieldList original_fields;
    parse_fix_fields(original_message, original_fields);

    std::string original_sending_time;
    for (size_t i = 0; i < original_fields.size(); ++i) {
        if (original_fields[i].first == 52) {
            original_sending_time = original_fields[i].second;
            break;
        }
    }

    FixMessage::FieldList fields;
    fields.reserve(original_fields.size() + 2);

    bool has_poss_dup = false;
    bool has_orig_sending_time = false;

    for (size_t i = 0; i < original_fields.size(); ++i) {
        if (original_fields[i].first == 43) {
            has_poss_dup = true;
        }
        if (original_fields[i].first == 122) {
            has_orig_sending_time = true;
        }
    }

    for (size_t i = 0; i < original_fields.size(); ++i) {
        const int tag = original_fields[i].first;

        if (tag == 8 || tag == 9 || tag == 10) {
            continue;
        }

        if (tag == 43) {
            fields.push_back(FixMessage::Field(43, "Y"));
            continue;
        }

        if (tag == 52) {
            fields.push_back(FixMessage::Field(52, utils::get_utc_timestamp()));

            if (!has_orig_sending_time && !original_sending_time.empty()) {
                fields.push_back(FixMessage::Field(122, original_sending_time));
            }
            continue;
        }

        if (tag == 122) {
            fields.push_back(FixMessage::Field(122, original_sending_time));
            continue;
        }

        fields.push_back(original_fields[i]);

        if (tag == 34 && !has_poss_dup) {
            fields.push_back(FixMessage::Field(43, "Y"));
        }
    }

    return fix.build_from_fields(fields);
}

static bool send_sequence_reset_gap_fill(TcpSocket& socket,
                                         FixMessage& fix,
                                         int msg_seq_num,
                                         int new_seq_no,
                                         uint64_t& last_send_ms) {
    const std::string gap_fill = fix.build_sequence_reset(msg_seq_num,
                                                          utils::get_utc_timestamp(),
                                                          new_seq_no,
                                                          true);

    return send_fix_message(socket, gap_fill, last_send_ms);
}

static bool handle_resend_request(TcpSocket& socket,
                                  FixMessage& fix,
                                  SequenceState& sequence_state,
                                  SentMessageStore& sent_messages,
                                  uint64_t& last_send_ms,
                                  const std::string& inbound_message) {
    int begin_seq_no = 0;
    int end_seq_no = 0;

    if (!get_int_tag(inbound_message, "7=", begin_seq_no)) {
        std::printf("Error: ResendRequest missing BeginSeqNo Tag7\n");
        return false;
    }

    if (!get_int_tag(inbound_message, "16=", end_seq_no)) {
        std::printf("Error: ResendRequest missing EndSeqNo Tag16\n");
        return false;
    }

    if (end_seq_no == 0 || end_seq_no >= sequence_state.outbound_seq) {
        end_seq_no = sequence_state.outbound_seq - 1;
    }

    if (end_seq_no < begin_seq_no) {
        return true;
    }

    std::printf("Info: handling ResendRequest from %d to %d\n",
                begin_seq_no,
                end_seq_no);

    for (int seq = begin_seq_no; seq <= end_seq_no; ++seq) {
        SentMessageStore::const_iterator it = sent_messages.find(seq);

        if (it == sent_messages.end()) {
            if (!send_sequence_reset_gap_fill(socket, fix, seq, seq + 1, last_send_ms)) {
                return false;
            }
            continue;
        }

        std::string msg_type;
        utils::find_tag_value(it->second, "35=", msg_type);

        if (is_admin_message(msg_type)) {
            if (!send_sequence_reset_gap_fill(socket, fix, seq, seq + 1, last_send_ms)) {
                return false;
            }
            continue;
        }

        const std::string poss_dup = build_poss_dup_message(fix, it->second);
        if (!send_fix_message(socket, poss_dup, last_send_ms)) {
            return false;
        }
    }

    return true;
}

static bool validate_inbound_sequence(TcpSocket& socket,
                                      FixMessage& fix,
                                      SequenceState& sequence_state,
                                      SentMessageStore& sent_messages,
                                      uint64_t& last_send_ms,
                                      const std::string& inbound_message,
                                      bool& stop_requested) {
    int received_seq = 0;
    if (!get_int_tag(inbound_message, "34=", received_seq)) {
        std::printf("Error: inbound message missing MsgSeqNum Tag34\n");
        stop_requested = true;
        return true;
    }

    if (received_seq > sequence_state.inbound_seq) {
        std::printf("Info: inbound sequence gap detected. Expected %d but received %d\n",
                    sequence_state.inbound_seq,
                    received_seq);

        const std::string resend_request =
            fix.build_resend_request(sequence_state.outbound_seq,
                                     utils::get_utc_timestamp(),
                                     sequence_state.inbound_seq,
                                     received_seq - 1);

        if (!send_current_message(socket,
                                  resend_request,
                                  last_send_ms,
                                  sequence_state,
                                  sent_messages)) {
            return false;
        }

        // This simple client processes the current message and accepts older resent
        // messages later only when PossDupFlag(43)=Y.
        sequence_state.inbound_seq = received_seq + 1;
        save_sequence_token(sequence_state);
        return true;
    }

    if (received_seq < sequence_state.inbound_seq) {
        std::string poss_dup_flag;
        utils::find_tag_value(inbound_message, "43=", poss_dup_flag);

        if (poss_dup_flag == "Y") {
            std::printf("Info: received possible duplicate message seq=%d\n", received_seq);
            return true;
        }

        std::printf("Error: inbound sequence too low. Expected %d but received %d\n",
                    sequence_state.inbound_seq,
                    received_seq);
        stop_requested = true;
        return true;
    }

    sequence_state.inbound_seq++;
    save_sequence_token(sequence_state);
    return true;
}

static bool process_inbound_message(TcpSocket& socket,
                                    FixMessage& fix,
                                    SequenceState& sequence_state,
                                    SentMessageStore& sent_messages,
                                    uint64_t& last_send_ms,
                                    const std::string& inbound_message,
                                    bool& logon_accepted,
                                    bool& stop_requested,
                                    bool scenarios_sent,
                                    bool& scenario_response_started,
                                    uint64_t& last_scenario_response_ms,
                                    bool& logout_initiated) {

    if (!is_running_regression) {
        std::printf("<< %s\n", utils::to_pipe_delimited(inbound_message).c_str());
    }

    if (!validate_inbound_sequence(socket,
                                   fix,
                                   sequence_state,
                                   sent_messages,
                                   last_send_ms,
                                   inbound_message,
                                   stop_requested)) {
        return false;
    }

    if (stop_requested) {
        return true;
    }

    std::string msg_type;
    if (!utils::find_tag_value(inbound_message, "35=", msg_type)) {
        return true;
    }

    if (msg_type == "4") {
        int new_seq_no = 0;
        if (get_int_tag(inbound_message, "36=", new_seq_no)) {
            if (new_seq_no > sequence_state.inbound_seq) {
                sequence_state.inbound_seq = new_seq_no;
                save_sequence_token(sequence_state);
                std::printf("Info: inbound sequence updated by SequenceReset to %d\n",
                            sequence_state.inbound_seq);
            }
        }
        return true;
    }

    if (msg_type == "2") {
        return handle_resend_request(socket,
                                     fix,
                                     sequence_state,
                                     sent_messages,
                                     last_send_ms,
                                     inbound_message);
    }

    if (!logon_accepted && msg_type == "A") {
        logon_accepted = true;
        return true;
    }

    // Initiate Logout Handsake after scenario finished.
    if (scenarios_sent && !logout_initiated) {
        if (!is_admin_message(msg_type)) {
            scenario_response_started = true;
            last_scenario_response_ms = utils::get_monotonic_millis();
        }
    }

    // TestRequest (35=1) -> Heartbeat (35=0) with same 112 (if present)
    if (msg_type == "1") {
        std::string test_req_id;
        utils::find_tag_value(inbound_message, "112=", test_req_id);

        const std::string heartbeat = fix.build_heartbeat(sequence_state.outbound_seq,
                                                          utils::get_utc_timestamp(),
                                                          test_req_id);

        return send_current_message(socket,
                                    heartbeat,
                                    last_send_ms,
                                    sequence_state,
                                    sent_messages);
    }

    // Logout (35=5) -> reply Logout and stop
    if (msg_type == "5") {
        if (!logout_initiated) {
            const std::string logout = fix.build_logout(sequence_state.outbound_seq,
                                                        utils::get_utc_timestamp(),
                                                        "");

            send_current_message(socket,
                                 logout,
                                 last_send_ms,
                                 sequence_state,
                                 sent_messages);
        }

        stop_requested = true;
        return true;
    }

    return true;
}

// Load custom RAW FIX messages from template file.
static bool run_scenarios(TcpSocket& socket, FixMessage& fix,
                          const SessionConfig& config,
                          const std::string& scenario_path,
                          SequenceState& sequence_state,
                          SentMessageStore& sent_messages,
                          uint64_t& last_send_ms,
                          bool& scenarios_sent) {

    scenarios_sent = false;
    std::vector<std::string> files;

    DIR* dir = ::opendir(scenario_path.c_str());
    if (dir) {
        dirent* entry = 0;
        while ((entry = ::readdir(dir)) != 0) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }

            if (!name.empty() && name[0] == '.') {
                continue;
            }

            files.push_back(scenario_path + "/" + name);
        }

        ::closedir(dir);
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(scenario_path);
    }

    for (size_t i = 0; i < files.size(); i++) {
        const std::string& file_path = files[i];

        std::ifstream in(file_path.c_str());
        if (!in.is_open()) {
            continue;
        }

        FixTemplateRuntime runtime;
        runtime.begin_string = config.begin_string;
        runtime.sender_comp_id = config.sender_comp_id;
        runtime.target_comp_id = config.target_comp_id;
        runtime.msg_seq_num = 0;
        runtime.sending_time_utc.clear();
        runtime.state.org_clord_id.clear();

        std::string line;
        while (std::getline(in, line)) {
            line = utils::trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            FixTemplateMessage template_message;
            template_message.msg_type.clear();
            template_message.fields.clear();

            size_t pos = 0;
            while (pos < line.size()) {
                size_t end = line.find('|', pos);
                if (end == std::string::npos) {
                    end = line.size();
                }

                const std::string field_text = line.substr(pos, end - pos);
                pos = (end < line.size()) ? (end + 1) : end;

                if (field_text.empty()) {
                    continue;
                }

                const size_t eq = field_text.find('=');
                if (eq == std::string::npos) {
                    continue;
                }

                const std::string tag_text = field_text.substr(0, eq);
                const std::string value_text = field_text.substr(eq + 1);

                const int tag_value = std::atoi(tag_text.c_str());
                if (tag_value <= 0) {
                    continue;
                }

                template_message.fields.push_back(std::make_pair(tag_value, value_text));
                if (tag_value == 35 && template_message.msg_type.empty()) {
                    template_message.msg_type = value_text;
                }
            }

            if (template_message.fields.empty()) {
                continue;
            }

            runtime.msg_seq_num = sequence_state.outbound_seq;
            runtime.sending_time_utc = utils::get_utc_timestamp();

            fix_template_apply(runtime, template_message);
            const std::string raw_fix = fix.build_from_fields(template_message.fields);

            if (!send_current_message(socket,
                                      raw_fix,
                                      last_send_ms,
                                      sequence_state,
                                      sent_messages)) {
                return false;
            }

            scenarios_sent = true;
        }
    }

    return true;
}

bool read_next_business_message(TcpSocket& socket,
                                FixParser& fix_parser,
                                FixMessage& fix,
                                SequenceState& sequence_state,
                                SentMessageStore& sent_messages,
                                uint64_t& last_send_ms,
                                bool& logon_accepted,
                                bool& stop_requested,
                                bool scenarios_sent,
                                bool& scenario_response_started,
                                uint64_t& last_scenario_response_ms,
                                bool& logout_initiated,
                                int timeout_ms,
                                std::string& out_message) {
    out_message.clear();

    const uint64_t start_ms = utils::get_monotonic_millis();
    char receive_buffer[receive_buffer_size];

    while (utils::get_monotonic_millis() - start_ms < static_cast<uint64_t>(timeout_ms)) {
        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            if (!process_inbound_message(socket,
                                         fix,
                                         sequence_state,
                                         sent_messages,
                                         last_send_ms,
                                         inbound_message,
                                         logon_accepted,
                                         stop_requested,
                                         scenarios_sent,
                                         scenario_response_started,
                                         last_scenario_response_ms,
                                         logout_initiated)) {
                return false;
            }

            if (stop_requested) {
                out_message = inbound_message;
                return true;
            }

            std::string msg_type;
            if (!utils::find_tag_value(inbound_message, "35=", msg_type)) {
                continue;
            }

            if (!is_admin_message(msg_type)) {
                out_message = inbound_message;
                return true;
            }
        }

        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            return false;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            return false;
        }

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));
    }

    return true;
}

int Application::run(const AppArgs& args) {
    ConfigParser config_parser;
    config_parser.load(args.config_path);

    SessionConfig config = config_parser.get_session(args.session_name);

    if (config.heartbeat_interval <= 0) {
        std::printf("Error: heartbeat_interval must be > 0 in config\n");
        return 1;
    }

    if (!socket.connect(config.host, config.port)) {
        std::printf("Error: Connection failed\n");
        return 1;
    }

    std::printf("Info: Connected to %s:%d\n", config.host.c_str(), config.port);

    const int sock_fd = socket.get_fd();
    if (sock_fd < 0) {
        std::printf("Error: invalid socket\n");
        socket.close();
        return 1;
    }

    if (!set_socket_recv_timeout(sock_fd, receive_timeout_millis)) {
        std::printf("Error: failed to set SO_RCVTIMEO\n");
        socket.close();
        return 1;
    }

    FixMessage fix;
    fix.set_begin_string(config.begin_string);
    fix.set_sender_comp_id(config.sender_comp_id);
    fix.set_target_comp_id(config.target_comp_id);

    FixParser fix_parser;

    const uint64_t heartbeat_interval_ms =
        static_cast<uint64_t>(config.heartbeat_interval) * 1000ULL;

    uint64_t last_send_ms = utils::get_monotonic_millis();
    uint64_t last_recv_ms = last_send_ms;

    uint64_t test_request_sent_ms = 0;
    int test_request_counter = 1;

    SequenceState sequence_state;
    SentMessageStore sent_messages;

    const std::string now_utc = utils::get_utc_timestamp();
    if (!read_sequence_token("tokens",
                             config.sender_comp_id,
                             now_utc,
                             config.reset_on_logon,
                             sequence_state)) {
        std::printf("ERROR: Token read failed\n");
        socket.close();
        return 1;
    }

    resend_store_path = get_resend_store_path(sequence_state.token_path);
    load_resend_store(resend_store_path, sent_messages);

    std::printf("Info: Sequence token %s OUT=%d IN=%d\n",
                sequence_state.token_path.c_str(),
                sequence_state.outbound_seq,
                sequence_state.inbound_seq);
    std::printf("Info: Resend store %s\n", resend_store_path.c_str());

    bool scenarios_sent = false;
    bool logout_initiated = false;
    uint64_t logout_start_ms = 0;

    bool scenario_response_started = false;
    uint64_t last_scenario_response_ms = 0;
    const uint64_t scenario_quiet_ms = 300ULL;

    uint64_t scenario_sent_ms = 0;
    const uint64_t scenario_first_response_timeout_ms = 5000ULL;

    char receive_buffer[receive_buffer_size];

    const std::string logon = fix.build_logon(sequence_state.outbound_seq,
                                              utils::get_utc_timestamp(),
                                              config.heartbeat_interval,
                                              config.reset_on_logon);

    if (!send_current_message(socket,
                              logon,
                              last_send_ms,
                              sequence_state,
                              sent_messages)) {
        socket.close();
        return 1;
    }

    const uint64_t logon_start_ms = utils::get_monotonic_millis();
    const uint64_t logon_timeout_ms =
        static_cast<uint64_t>(logon_timeout_seconds) * 1000ULL;

    bool logon_accepted = false;

    while (!logon_accepted) {
        const uint64_t now_ms = utils::get_monotonic_millis();
        if (now_ms - logon_start_ms >= logon_timeout_ms) {
            std::printf("Error: logon timeout (no 35=A)\n");
            socket.close();
            return 1;
        }

        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            std::printf("Info: peer closed\n");
            socket.close();
            return 1;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            std::printf("Error: receive failed\n");
            socket.close();
            return 1;
        }

        last_recv_ms = utils::get_monotonic_millis();
        test_request_sent_ms = 0;

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));

        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            bool stop_requested = false;

            if (!process_inbound_message(socket,
                                         fix,
                                         sequence_state,
                                         sent_messages,
                                         last_send_ms,
                                         inbound_message,
                                         logon_accepted,
                                         stop_requested,
                                         scenarios_sent,
                                         scenario_response_started,
                                         last_scenario_response_ms,
                                         logout_initiated)) {
                socket.close();
                return 1;
            }

            if (stop_requested) {
                socket.close();
                return 0;
            }

            if (logon_accepted) {
                break;
            }
        }
    }

    if (args.is_test_mode) {
        is_running_regression = true;

        if (!run_fix_regression(socket,
                                fix_parser,
                                fix,
                                args.scenario_path,
                                sequence_state,
                                sent_messages,
                                last_send_ms,
                                logon_accepted,
                                scenarios_sent,
                                scenario_response_started,
                                last_scenario_response_ms,
                                logout_initiated)) {
            is_running_regression = false;
            socket.close();
            return 1;
        }

        is_running_regression = false;
    }
    else {
        if (!run_scenarios(socket,
                           fix,
                           config,
                           args.scenario_path,
                           sequence_state,
                           sent_messages,
                           last_send_ms,
                           scenarios_sent)) {
            socket.close();
            return 1;
        }
    }

    scenario_sent_ms = utils::get_monotonic_millis();

    logon_accepted = true;
    while (true) {
        const uint64_t now_ms = utils::get_monotonic_millis();

        if (!logout_initiated && scenarios_sent && !scenario_response_started) {
            if (now_ms - scenario_sent_ms >= scenario_first_response_timeout_ms) {
                const std::string logout = fix.build_logout(sequence_state.outbound_seq,
                                                            utils::get_utc_timestamp(),
                                                            "");

                if (!send_current_message(socket,
                                          logout,
                                          last_send_ms,
                                          sequence_state,
                                          sent_messages)) {
                    break;
                }

                logout_initiated = true;
                logout_start_ms = now_ms;
            }
        }

        if (!logout_initiated && scenarios_sent && scenario_response_started) {
            if (now_ms - last_scenario_response_ms >= scenario_quiet_ms) {
                const std::string logout = fix.build_logout(sequence_state.outbound_seq,
                                                            utils::get_utc_timestamp(),
                                                            "");

                if (!send_current_message(socket,
                                          logout,
                                          last_send_ms,
                                          sequence_state,
                                          sent_messages)) {
                    break;
                }

                logout_initiated = true;
                logout_start_ms = now_ms;
            }
        }

        if (logout_initiated) {
            if (now_ms - logout_start_ms >= 2000ULL) {
                std::printf("Info: logout wait timeout, closing\n");
                break;
            }
        }
        else {
            if (test_request_sent_ms != 0) {
                if (now_ms - test_request_sent_ms >= heartbeat_interval_ms) {
                    std::printf("Error: TestRequest timeout\n");
                    break;
                }
            } else {
                if (now_ms - last_recv_ms >= heartbeat_interval_ms) {
                    char test_req_id_buf[32];
                    std::snprintf(test_req_id_buf, sizeof(test_req_id_buf), "TR%d", test_request_counter++);
                    const std::string test_req_id(test_req_id_buf);

                    const std::string test_request = fix.build_test_request(sequence_state.outbound_seq,
                                                                            utils::get_utc_timestamp(),
                                                                            test_req_id);

                    if (!send_current_message(socket,
                                              test_request,
                                              last_send_ms,
                                              sequence_state,
                                              sent_messages)) {
                        break;
                    }

                    test_request_sent_ms = utils::get_monotonic_millis();
                }
            }

            if (now_ms - last_send_ms >= heartbeat_interval_ms) {
                const std::string heartbeat = fix.build_heartbeat(sequence_state.outbound_seq,
                                                                  utils::get_utc_timestamp(),
                                                                  "");

                if (!send_current_message(socket,
                                          heartbeat,
                                          last_send_ms,
                                          sequence_state,
                                          sent_messages)) {
                    break;
                }
            }
        }

        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            std::printf("Info: peer closed\n");
            break;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            std::printf("Error: receive failed\n");
            break;
        }

        last_recv_ms = utils::get_monotonic_millis();
        test_request_sent_ms = 0;

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));

        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            bool stop_requested = false;

            if (!process_inbound_message(socket,
                                         fix,
                                         sequence_state,
                                         sent_messages,
                                         last_send_ms,
                                         inbound_message,
                                         logon_accepted,
                                         stop_requested,
                                         scenarios_sent,
                                         scenario_response_started,
                                         last_scenario_response_ms,
                                         logout_initiated)) {
                socket.close();
                return 1;
            }

            if (stop_requested) {
                socket.close();
                return 0;
            }
        }
    }

    socket.close();
    return 0;
}
