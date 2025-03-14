#include "chat-parser.h"
#include "common.h"
#include "log.h"
#include "regex-partial.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

common_chat_msg_parser::common_chat_msg_parser(const std::string & input, bool is_partial, const common_chat_syntax & syntax)
    : input_(input), is_partial_(is_partial), syntax_(syntax)
{
    result_.role = "assistant";

    while (true) {
        std::string id = std::to_string(std::rand());
        if (input.find(id) == std::string::npos) {
            healing_marker_ = id;
            break;
        }
    }
}

std::string common_chat_msg_parser::str(const common_string_range & rng) const {
    GGML_ASSERT(rng.begin <= rng.end);
    return input_.substr(rng.begin, rng.end - rng.begin);
}

void common_chat_msg_parser::add_content(const std::string &content) {
    result_.content += content;
}

void common_chat_msg_parser::add_reasoning_content(const std::string &reasoning_content) {
    result_.reasoning_content += reasoning_content;
}

bool common_chat_msg_parser::add_tool_call(const std::string & name, const std::string & id, const std::string & arguments, const common_healing_marker & healing_marker) {
    if (name.empty()) {
        return false;
    }

    auto marker_idx = std::string::npos;
    if (!arguments.empty() && !healing_marker.marker.empty()) {
        marker_idx = arguments.find(healing_marker.json_dump_marker);
        if (marker_idx == std::string::npos) {
            marker_idx = arguments.find(healing_marker.marker);
        }
    }

    common_chat_tool_call tool_call;
    tool_call.name = name;
    tool_call.arguments = marker_idx != std::string::npos ? arguments.substr(0, marker_idx) : arguments;
    tool_call.id = id;

    if (tool_call.arguments == "\"") {
        // This happens because of completing `:"$magic` after `"arguments"`
        tool_call.arguments = "";
    }
    LOG_DBG("Tool call arguments:\n\traw: %s\n\tresult: %s\n", arguments.c_str(), tool_call.arguments.c_str());
    result_.tool_calls.emplace_back(tool_call);
    return true;
}
bool common_chat_msg_parser::add_tool_call(const json & tool_call, const common_healing_marker & healing_marker) {
    std::string name = tool_call.contains("name") ? tool_call.at("name") : "";
    std::string id = tool_call.contains("id") ? tool_call.at("id") : "";
    std::string arguments = tool_call.contains("arguments") ? tool_call.at("arguments").dump() : "";
    return add_tool_call(name, id, arguments, healing_marker);
}

bool common_chat_msg_parser::add_tool_calls(const json & arr, const common_healing_marker & healing_marker) {
    for (const auto & item : arr) {
        if (!add_tool_call(item, healing_marker)) {
            return false;
        }
    }
    return true;
}
void common_chat_msg_parser::finish() {
    if (!is_partial_ && pos_ != input_.size()) {
        throw std::runtime_error("Unexpected content at end of input: " + input_.substr(pos_));
    }
    result_.reasoning_content = string_strip(result_.reasoning_content);
    if (!result_.tool_calls.empty()) {
        result_.content = string_strip(result_.content);
    }
}

[[noreturn]]
void common_chat_msg_parser::incomplete(const std::string & message) {
    if (is_partial_) {
        finish();
    }
    throw common_chat_msg_partial_exception(message);
}

bool common_chat_msg_parser::consume_spaces() {
    const auto length = input_.size();
    auto consumed = false;
    while (pos_ < length && std::isspace(input_[pos_])) {
        ++pos_;
        consumed = true;
    }
    return consumed;
}

bool common_chat_msg_parser::try_consume_literal(const std::string & literal) {
    auto pos = pos_;
    for (auto i = 0u; i < literal.size(); ++i) {
        if (pos >= input_.size()) {
            return false;
        }
        if (input_[pos] != literal[i]) {
            return false;
        }
        ++pos;
    }
    pos_ = pos;
    return true;
}

void common_chat_msg_parser::consume_literal(const std::string & literal) {
    if (!try_consume_literal(literal)) {
        incomplete("Expected literal '" + literal + "' at position " + std::to_string(pos_));
    }
}

void common_chat_msg_parser::try_consume_think_tags(const common_regex & start_think_regex, const common_regex & end_think_regex) {
    if (syntax_.reasoning_format != COMMON_REASONING_FORMAT_NONE) {
        if (syntax_.thinking_forced_open || try_consume_regex(start_think_regex)) {
            if (auto res = try_find_regex(end_think_regex)) {
                result_.reasoning_content = res->prelude;
                consume_spaces();
            } else {
                result_.reasoning_content = consume_rest();
                if (!syntax_.thinking_forced_open) {
                    incomplete("Failed to find end of reasoning tag " + end_think_regex.str());
                }
                return;
            }
        } else if (auto res = try_find_regex(end_think_regex)) {
            result_.reasoning_content = res->prelude;
            consume_spaces();
        }
    }
}

std::string common_chat_msg_parser::consume_rest() {
    auto rest = input_.substr(pos_);
    pos_ = input_.size();
    return rest;
}

// Tries to find the regex, consumes it (pos right after it) and gives the prelude (right before it) and the groups to the callback.
std::optional<common_chat_msg_parser::find_regex_result> common_chat_msg_parser::try_find_regex(const common_regex & regex, size_t from) {
    auto m = regex.search(input_, from == std::string::npos ? pos_ : from);
    if (m.type == COMMON_REGEX_MATCH_TYPE_NONE) {
        return std::nullopt;
    }
    if (m.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
        if (is_partial()) {
            incomplete(regex.str());
        }
        return std::nullopt;
    }
    auto prelude = input_.substr(pos_, m.groups[0].begin - pos_);
    pos_ = m.groups[0].end;

    return find_regex_result{prelude, m.groups};
}

common_chat_msg_parser::consume_regex_result common_chat_msg_parser::consume_regex(const common_regex & regex) {
    if (auto result = try_consume_regex(regex)) {
        return *result;
    }
    incomplete("Failed to consume regex: " + regex.str());
}

std::optional<common_chat_msg_parser::consume_regex_result> common_chat_msg_parser::try_consume_regex(const common_regex & regex) {
    auto m = regex.search(input_, pos_);
    if (m.type == COMMON_REGEX_MATCH_TYPE_NONE) {
        return std::nullopt;
    }
    if (m.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
        incomplete(regex.str());
    }
    if (m.groups[0].begin != pos_) {
        // Didn't match at the current position.
        return std::nullopt;
    }
    pos_ = m.groups[0].end;

    return consume_regex_result{m.groups};
}

// Calls the callback, *then* explodes w/ a partial match exception if it's partial
common_json common_chat_msg_parser::consume_json(
    const std::vector<std::vector<std::string>> & args_paths
) {
    if (auto result = try_consume_json(args_paths)) {
        return *result;
    }
    incomplete("Failed to consume JSON");
}

std::optional<common_json> common_chat_msg_parser::try_consume_json(
    const std::vector<std::vector<std::string>> & args_paths
) {
    auto it = input_.cbegin() + pos_;
    const auto end = input_.cend();
    common_json result;
    if (!common_json_parse(it, end, healing_marker_, result)) {
        return std::nullopt;
    }
    pos_ = std::distance(input_.cbegin(), it);
    if (result.healing_marker.marker.empty()) {
        // No healing marker, just return the parsed json
        return result;
    }
    if (!is_partial()) {
        incomplete("JSON is incomplete");
    }

    LOG_DBG("Parsed partial JSON: %s (json_healing_marker: %s)\n", result.json.dump().c_str(), result.healing_marker.json_dump_marker.c_str());

    // Healing marker found, we need to visit the json and removed objects that we didn't want to heal
    auto is_arguments_path = [&](const std::vector<std::string> & path) {
        return std::find(args_paths.begin(), args_paths.end(), path) != args_paths.end();
    };

    std::vector<std::string> path;
    std::function<json(const json &)> remove_unsupported_healings = [&](const json & j) {
        if (j.is_object()) {
            auto obj = json::object();
            for (const auto & p : j.items()) {
                const auto & key = p.key();
                const auto & value = p.value();
                const std::string key_str = key; // NOLINT
                auto idx = key_str.find(healing_marker_);
                if (idx != std::string::npos) {//} && idx != 0) {
                    // Don't heal keys halfway, cut just after their opening quotes
                    obj[result.healing_marker.marker] = 1;
                    if (idx != 0) {
                        result.healing_marker.json_dump_marker = result.healing_marker.marker;
                    }
                    break;
                }
                path.push_back(key_str);
                auto is_args = is_arguments_path(path);
                if (is_args) {
                    obj[key] = value;
                } else if (value.is_string()) {
                    const std::string value_str = value;
                    if (value_str.find(healing_marker_) == std::string::npos) {
                        obj[key] = value;
                    } else {
                        obj[result.healing_marker.marker] = 1;
                        result.healing_marker.json_dump_marker = result.healing_marker.marker;
                    }
                } else {
                    obj[key] = remove_unsupported_healings(value);
                }
                path.pop_back();
            }
            return obj;
        }
        if (j.is_array()) {
            auto arr = json::array();
            for (const auto & value : j) {
                if (value.is_string()) {
                    std::string str = value;
                    auto idx = str.find(healing_marker_);
                    if (idx != std::string::npos) {
                        // Don't heal array values that aren't in the arguments.
                        arr.push_back(result.healing_marker.marker);
                        result.healing_marker.json_dump_marker = result.healing_marker.marker;
                        break;
                    }
                }
                arr.push_back(remove_unsupported_healings(value));
            }
            return arr;
        }
        return j;
    };

    if (!is_arguments_path({})) {
        auto cleaned = remove_unsupported_healings(result.json);
        LOG_DBG("Cleaned up JSON %s to %s (json_healing_marker : '%s')\n", result.json.dump().c_str(), cleaned.dump().c_str(), result.healing_marker.json_dump_marker.c_str());
        result.json = cleaned;
    }
    LOG_DBG("Half-healed json: %s\n", result.json.dump().c_str());
    return result;
}
