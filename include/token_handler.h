#ifndef TOKEN_HANDLER_H
#define TOKEN_HANDLER_H

#include <string>
#include "fix_session_state.h"

bool read_sequence_token(const std::string& token_dir,
                         const std::string& sender_comp_id,
                         const std::string& utc_timestamp,
                         bool reset_on_logon,
                         SequenceState& sequence_state);

bool save_sequence_token(const std::string& token_path,
                         const SequenceState& sequence_state);

bool save_sequence_token(const SequenceState& sequence_state);

// Backward-compatible helpers for older code paths.
bool read_token(const std::string& token_dir,
                const std::string& sender_comp_id,
                const std::string& utc_timestamp,
                bool reset_on_logon,
                int& next_seq,
                std::string& token_path_out);

bool save_token(const std::string& token_path, int next_seq);

#endif
