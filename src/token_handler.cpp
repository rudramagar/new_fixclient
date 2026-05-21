#include "token_handler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static bool ensure_token_dir(const std::string& dir) {
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0755) != 0) {
            return errno == EEXIST;
        }
        return true;
    }

    return S_ISDIR(st.st_mode);
}

static bool parse_positive_number(const char* text, int& value) {
    if (!text) {
        return false;
    }

    char* end_ptr = 0;
    const long number = std::strtol(text, &end_ptr, 10);

    if (end_ptr == text || number <= 0 || number > 2000000000L) {
        return false;
    }

    value = static_cast<int>(number);
    return true;
}

bool save_sequence_token(const std::string& token_path,
                         const SequenceState& sequence_state) {
    if (token_path.empty()) {
        return false;
    }

    if (sequence_state.outbound_seq <= 0 || sequence_state.inbound_seq <= 0) {
        return false;
    }

    std::FILE* file = std::fopen(token_path.c_str(), "w");
    if (!file) {
        return false;
    }

    std::fprintf(file, "%010d : %010d\n",
                 sequence_state.outbound_seq,
                 sequence_state.inbound_seq);

    std::fclose(file);
    return true;
}

bool save_sequence_token(const SequenceState& sequence_state) {
    return save_sequence_token(sequence_state.token_path, sequence_state);
}

bool read_sequence_token(const std::string& token_dir,
                         const std::string& sender_comp_id,
                         const std::string& utc_timestamp,
                         bool reset_on_logon,
                         SequenceState& sequence_state) {
    sequence_state.outbound_seq = 1;
    sequence_state.inbound_seq = 1;
    sequence_state.token_path.clear();

    if (sender_comp_id.empty()) {
        return false;
    }

    std::string dir = token_dir;
    if (dir.empty()) {
        dir = "tokens";
    }

    if (!ensure_token_dir(dir)) {
        return false;
    }

    std::string day = "00000000";
    if (utc_timestamp.size() >= 8) {
        day = utc_timestamp.substr(0, 8);
    }

    sequence_state.token_path = dir + "/" + sender_comp_id + "_" + day + ".token";

    if (reset_on_logon) {
        return save_sequence_token(sequence_state);
    }

    std::FILE* file = std::fopen(sequence_state.token_path.c_str(), "r");
    if (!file) {
        return save_sequence_token(sequence_state);
    }

    char buf[128];
    const size_t bytes_read = std::fread(buf, 1, sizeof(buf) - 1, file);
    std::fclose(file);
    buf[bytes_read] = '\0';

    char* colon = std::strchr(buf, ':');
    if (colon) {
        *colon = '\0';

        int outbound_seq = 0;
        int inbound_seq = 0;

        if (parse_positive_number(buf, outbound_seq) &&
            parse_positive_number(colon + 1, inbound_seq)) {
            sequence_state.outbound_seq = outbound_seq;
            sequence_state.inbound_seq = inbound_seq;
            return true;
        }
    }
    else {
        // Backward compatibility: old token format contained only outbound seq.
        int outbound_seq = 0;
        if (parse_positive_number(buf, outbound_seq)) {
            sequence_state.outbound_seq = outbound_seq;
            sequence_state.inbound_seq = 1;
            return save_sequence_token(sequence_state);
        }
    }

    sequence_state.outbound_seq = 1;
    sequence_state.inbound_seq = 1;
    return save_sequence_token(sequence_state);
}

bool save_token(const std::string& token_path, int next_seq) {
    if (token_path.empty()) {
        return false;
    }

    if (next_seq <= 0) {
        return false;
    }

    std::FILE* file = std::fopen(token_path.c_str(), "w");
    if (!file) {
        return false;
    }

    std::fprintf(file, "%d\n", next_seq);
    std::fclose(file);
    return true;
}

bool read_token(const std::string& token_dir,
                const std::string& sender_comp_id,
                const std::string& utc_timestamp,
                bool reset_on_logon,
                int& next_seq,
                std::string& token_path_out) {
    SequenceState sequence_state;

    if (!read_sequence_token(token_dir,
                             sender_comp_id,
                             utc_timestamp,
                             reset_on_logon,
                             sequence_state)) {
        return false;
    }

    next_seq = sequence_state.outbound_seq;
    token_path_out = sequence_state.token_path;
    return true;
}
