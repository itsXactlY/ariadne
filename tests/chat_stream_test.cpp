// ChatStream: SSE chat.completion.chunk → Message. The first case is a real
// Strata 0.1.37 stream (reasoning_effort "low", 2026-10-20), fed at every
// possible split point.
#include "dd/agent/chat_stream.hpp"

#include <cstdio>
#include <string>

using dd::agent::ChatStream;
using dd::core::Json;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

static const char* kStrata = R"SSE(data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"role": "assistant", "content": ""}, "finish_reason": null}]}

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"reasoning_content": "The"}, "finish_reason": null}]}

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"reasoning_content": " user"}, "finish_reason": null}]}

: keep-alive

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"reasoning_content": "\n"}, "finish_reason": null}]}

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"content": "2"}, "finish_reason": null}]}

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {"content": " +"}, "finish_reason": null}]}

data: {"id": "chatcmpl-250de6e65b7b4251b94c4c9e", "object": "chat.completion.chunk", "created": 1792472946, "model": "qwen3.8-flash-next-coder-iq1_m", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 44, "completion_tokens": 31, "total_tokens": 75, "prompt_tokens_details": {"cached_tokens": 0}}, "timings": {"cache_n": 0, "prompt_n": 44, "prompt_ms": 851.0, "prompt_per_token_ms": 19.341, "prompt_per_second": 51.7, "predicted_n": 31, "predicted_ms": 984.4, "predicted_per_token_ms": 31.755, "predicted_per_second": 31.5, "draft_n": 29, "draft_n_accepted": 21}}

data: [DONE]

)SSE";

static double num(const Json& o, const char* k) {
    const Json* v = dd::core::json_get(o, k);
    return v && v->type == Json::Num ? v->num : -1;
}

int main() {
    const std::string all = kStrata;
    for (size_t cut = 0; cut <= all.size(); ++cut) {
        ChatStream cs;
        std::string r, c;
        cs.on_reasoning = [&](const std::string& d) { r += d; };
        cs.on_content = [&](const std::string& d) { c += d; };
        cs.feed(all.data(), cut);
        cs.feed(all.data() + cut, all.size() - cut);
        cs.finish();
        const auto m = cs.message();
        const bool ok = r == "The user\n" && c == "2 +" && cs.reasoning() == r && m.content == c &&
                        m.finish_reason == "stop" && m.tool_calls.empty() && cs.error().empty() &&
                        num(cs.usage(), "prompt_tokens") == 44 && num(cs.timings(), "predicted_per_second") == 31.5;
        if (!ok) { chk(false, "strata stream split at " + std::to_string(cut)); break; }
    }

    {   // tool calls arrive in pieces, keyed by index
        ChatStream cs;
        const std::string s =
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c0\",\"type\":\"function\",\"function\":{\"name\":\"list_dir\",\"arguments\":\"\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"c1\",\"function\":{\"name\":\"read_\",\"arguments\":\"{\\\"pa\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"/x\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"function\":{\"name\":\"file\",\"arguments\":\"th\\\":\\\"/y\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n";
        cs.feed(s.data(), s.size());
        const auto m = cs.message();
        chk(m.tool_calls.size() == 2 && m.tool_calls[0].id == "c0" && m.tool_calls[0].name == "list_dir" &&
                m.tool_calls[0].arguments == "{\"path\":\"/x\"}" && m.tool_calls[1].name == "read_file" &&
                m.tool_calls[1].arguments == "{\"path\":\"/y\"}" && m.finish_reason == "tool_calls",
            "tool call deltas");
    }

    {   // an error chunk, CRLF lines, no trailing newline
        ChatStream cs;
        const std::string s = "data: {\"error\":{\"message\":\"Unexpected reasoning effort max\"}}\r\n\r\ndata: {\"choices\":[]}";
        cs.feed(s.data(), s.size());
        cs.finish();
        chk(cs.error() == "Unexpected reasoning effort max" && cs.saw_data(), "error chunk");
    }

    {   // not SSE at all: nothing parsed, raw kept for the caller's fallback
        ChatStream cs;
        const std::string s = "{\"choices\":[{\"message\":{\"content\":\"hi\"}}]}";
        cs.feed(s.data(), s.size());
        cs.finish();
        chk(!cs.saw_data() && cs.raw_head() == s, "plain json is not a stream");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all chat-stream tests passed\n");
    return g_fail ? 1 : 0;
}
