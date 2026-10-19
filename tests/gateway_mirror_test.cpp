// Differential test for dd::gateway::find_session_id (mirror.py matching).
#include "dd/gateway/mirror.hpp"
#include <iostream>
#include <string>
#include <optional>
#include <initializer_list>
#include "dd/core/json.hpp"

using namespace dd;
using namespace dd::core;
using namespace dd::gateway;

int fails = 0;
static void ck(const std::optional<std::string>& got, const std::string& want, const char* tag) {
    std::string g = got ? *got : "<none>";
    if (g != want) { ++fails; std::cout << tag << " got=" << g << " want=" << want << "\n"; }
}

static Json Jstr(const std::string& s){ Json j; j.type=Json::Str; j.str=s; return j; }
static Json Jnum(double n){ Json j; j.type=Json::Num; j.num=n; return j; }
static Json Jnull(){ Json j; j.type=Json::Null; return j; }

static Json obj(std::initializer_list<std::pair<std::string,Json>> ms){
    std::vector<std::pair<std::string,JsonSP>> w;
    for(auto& m : ms){ auto sp=std::make_shared<Json>(std::move(m.second)); w.push_back({m.first,sp}); }
    return json_obj(w);
}

int main(){
    Json index = obj({
      {"a", obj({{"origin", obj({{"platform",Jstr("Telegram")},{"chat_id",Jstr("c1")},{"thread_id",Jstr("t1")}})},
                   {"session_id",Jstr("S-tg-1")},{"updated_at",Jstr("2026-01-01")}})},
      {"b", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")},{"thread_id",Jstr("t2")}})},
                   {"session_id",Jstr("S-tg-2")},{"updated_at",Jstr("2026-02-01")}})},
      {"c", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")}})},
                   {"session_id",Jstr("S-tg-nodom")},{"updated_at",Jstr("2026-03-01")}})},
      {"d", obj({{"platform",Jstr("slack")},{"chat_id",Jstr("s9")},{"session_id",Jstr("S-slack")},{"updated_at",Jstr("2026-05-01")}})},
      {"e", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jnum(7)}})},
                   {"session_id",Jstr("S-tg-int")},{"updated_at",Jstr("2026-06-01")}})},
      {"f", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")}})},
                   {"session_id",Jstr("S-older")},{"updated_at",Jstr("2025-01-01")}})},
      {"h", obj({{"origin",Jnull()},{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")},
                   {"session_id",Jstr("S-originnull")},{"updated_at",Jstr("2026-09-01")}})},
      {"i", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")},{"thread_id",Jnull()}})},
                   {"session_id",Jstr("S-thnull")},{"updated_at",Jstr("2026-09-05")}})},
      {"j", obj({{"origin", obj({{"platform",Jstr("telegram")},{"chat_id",Jstr("c1")},{"thread_id",Jstr("t1")}})},
                   {"session_id",Jnull()},{"updated_at",Jstr("2026-09-06")}})},
    });

    ck(find_session_id("telegram","c1",std::nullopt,index), "<none>", "tc1_none");
    ck(find_session_id("Telegram","c1","t1",index), "<none>", "tc1_t1");
    ck(find_session_id("telegram","c1","t2",index), "S-tg-2", "tc1_t2");
    ck(find_session_id("TELEGRAM","s9",std::nullopt,index), "<none>", "slack_s9");
    ck(find_session_id("telegram","7",std::nullopt,index), "S-tg-int", "int_c");
    ck(find_session_id("telegram","c1","nope",index), "<none>", "nothread");
    ck(find_session_id("telegram","c1","t9",index), "<none>", "t9");
    ck(find_session_id("Telegram","c1",std::nullopt,index), "<none>", "TG_c1_none");

    Json empty; empty.type=Json::Obj;
    ck(find_session_id("telegram","c1",std::nullopt,empty), "<none>", "empty");

    std::cout << "mirror tests: " << (fails?"FAIL (":"PASS (") << fails << " failed)\n";
    return fails?1:0;
}
