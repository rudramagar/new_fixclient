#ifndef FIX_SESSION_STATE_H
#define FIX_SESSION_STATE_H

#include <map>
#include <string>

struct SequenceState {
    int outbound_seq;
    int inbound_seq;
    std::string token_path;

    SequenceState() : outbound_seq(1), inbound_seq(1) {}
};

typedef std::map<int, std::string> SentMessageStore;

#endif
