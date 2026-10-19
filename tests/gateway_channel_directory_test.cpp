// Differential test for dd::gateway channel_directory pure helpers.
#include "dd/gateway/channel_directory.hpp"
#include "dd/core/json.hpp"
#include <iostream>
#include <optional>
#include <string>

using namespace dd;
using namespace dd::core;
using namespace dd::gateway;

int fails = 0;

static Json obj(std::initializer_list<std::pair<std::string, Json>> ms) {
    std::vector<std::pair<std::string, JsonSP>> v;
    for (auto& m : ms) v.push_back({m.first, std::make_shared<Json>(m.second)});
    return json_obj(v);
}

static void eq_str(const std::string& got, const std::string& want, const char* tag) {
    if (got != want) {
        ++fails;
        std::cout << tag << " got=[" << got << "] want=[" << want << "]\n";
    }
}

int main() {
    // normalize_channel_query
    eq_str(normalize_channel_query("#foo"), "foo", "nq1");
    eq_str(normalize_channel_query("## Foo "), "foo", "nq2");
    eq_str(normalize_channel_query(""), "", "nq3");
    eq_str(normalize_channel_query("#"), "", "nq4");
    eq_str(normalize_channel_query("  #Bar  "), "#bar", "nq5");
    eq_str(normalize_channel_query("x#y"), "x#y", "nq6");
    eq_str(normalize_channel_query("##"), "", "nq7");

    // channel_target_name
    eq_str(channel_target_name("discord", obj({{"name", json_str("gen")}, {"guild", json_str("G")}})), "#gen", "t1");
    eq_str(channel_target_name("discord", obj({{"name", json_str("gen")}})), "gen", "t2");
    eq_str(channel_target_name("slack", obj({{"name", json_str("x")}, {"type", json_str("channel")}})), "x (channel)", "t3");
    eq_str(channel_target_name("slack", obj({{"name", json_str("x")}})), "x", "t4");
    eq_str(channel_target_name("telegram", obj({{"name", json_str("y")}, {"type", json_str("group")}})), "y (group)", "t5");

    // session_entry_id
    auto id0 = session_entry_id(obj({}));
    if (id0.has_value()) { ++fails; std::cout << "sid empty not-null\n"; }
    auto id1 = session_entry_id(obj({{"chat_id", json_null()}}));
    if (id1.has_value()) { ++fails; std::cout << "sid null not-null\n"; }
    auto id2 = session_entry_id(obj({{"chat_id", json_num(0)}}));
    if (id2.has_value()) { ++fails; std::cout << "sid 0 not-null\n"; }
    auto id3 = session_entry_id(obj({{"chat_id", json_str("c1")}}));
    if (!id3.has_value() || *id3 != "c1") { ++fails; std::cout << "sid c1\n"; }
    auto id4 = session_entry_id(obj({{"chat_id", json_str("c1")}, {"thread_id", json_str("t2")}}));
    if (!id4.has_value() || *id4 != "c1:t2") { ++fails; std::cout << "sid c1:t2\n"; }
    auto id5 = session_entry_id(obj({{"chat_id", json_num(5)}}));
    if (!id5.has_value() || *id5 != "5") { ++fails; std::cout << "sid 5\n"; }
    auto id6 = session_entry_id(obj({{"chat_id", json_num(5)}, {"thread_id", json_num(9)}}));
    if (!id6.has_value() || *id6 != "5:9") { ++fails; std::cout << "sid 5:9\n"; }
    auto id7 = session_entry_id(obj({{"chat_id", json_str("c")}, {"thread_id", json_num(0)}}));
    if (!id7.has_value() || *id7 != "c") { ++fails; std::cout << "sid c (thread 0)\n"; }
    auto id8 = session_entry_id(obj({{"chat_id", json_bool(true)}}));
    if (!id8.has_value() || *id8 != "True") { ++fails; std::cout << "sid True\n"; }

    // session_entry_name
    eq_str(session_entry_name(obj({})), "None", "sn0");
    eq_str(session_entry_name(obj({{"chat_name", json_str("N")}})), "N", "sn1");
    eq_str(session_entry_name(obj({{"chat_name", json_str("")}, {"user_name", json_str("U")}})), "U", "sn2");
    eq_str(session_entry_name(obj({{"user_name", json_str("U")}, {"chat_id", json_num(7)}})), "U", "sn3");
    eq_str(session_entry_name(obj({{"chat_id", json_num(7)}})), "7", "sn4");
    eq_str(session_entry_name(obj({{"chat_name", json_str("N")}, {"thread_id", json_str("t")}})), "N / topic t", "sn5");
    eq_str(session_entry_name(obj({{"chat_name", json_str("N")}, {"thread_id", json_str("t")}, {"chat_topic", json_str("Top")}})), "N / Top", "sn6");
    eq_str(session_entry_name(obj({{"user_name", json_str("U")}, {"thread_id", json_str("t")}})), "U / topic t", "sn7");
    eq_str(session_entry_name(obj({{"chat_id", json_num(7)}, {"thread_id", json_str("t")}, {"chat_topic", json_str("")}})), "7 / topic t", "sn8");
    eq_str(session_entry_name(obj({{"chat_name", json_num(0)}, {"user_name", json_num(0)}, {"chat_id", json_num(9)}})), "9", "sn9");

    std::cout << "channel_directory tests: " << (fails ? "FAIL" : "PASS")
              << " (" << fails << " failed)\n";
    return fails ? 1 : 0;
}
