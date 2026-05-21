#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include <string>
#include <map>

struct SessionConfig {
    std::string name;
    std::string host;
    int port = 0;
    std::string begin_string = "FIX.4.4";
    std::string sender_comp_id;
    std::string target_comp_id;
    int heartbeat_interval = 30;
    bool reset_on_logon = false;
};

class ConfigParser {
public:
    void load(const std::string& path);
    SessionConfig get_session(const std::string& session_name) const;

private:
    SessionConfig defaults;
    std::map<std::string, SessionConfig> sessions;
};

#endif

