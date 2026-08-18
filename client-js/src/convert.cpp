#include "convert.hpp"

namespace oxymp::client::js {

std::string fromJs(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (value.IsEmpty()) {
        return {};
    }

    v8::Local<v8::String> text;
    if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&text)) {
        return {};
    }

    // Длина в байтах, а не в знаках: строка UTF-16 в UTF-8 может стать длиннее
    // втрое, и места нужно ровно столько, сколько скажет сам движок.
    const std::size_t size = text->Utf8LengthV2(isolate);

    std::string collected(size, '\0');
    if (size != 0) {
        text->WriteUtf8V2(isolate, collected.data(), collected.size());
    }

    return collected;
}

} // namespace oxymp::client::js
