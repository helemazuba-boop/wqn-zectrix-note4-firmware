#include "text_render.h"

#include <cctype>

namespace {

void AppendDecodedEntity(const std::string& entity, std::string* output)
{
    if (entity == "amp") {
        output->push_back('&');
    } else if (entity == "lt") {
        output->push_back('<');
    } else if (entity == "gt") {
        output->push_back('>');
    } else if (entity == "quot") {
        output->push_back('"');
    } else if (entity == "apos") {
        output->push_back('\'');
    } else if (entity == "nbsp") {
        output->push_back(' ');
    } else {
        output->push_back('&');
        output->append(entity);
        output->push_back(';');
    }
}

}  // namespace

namespace wqn {

std::string HtmlToPlainText(const std::string& html)
{
    std::string output;
    output.reserve(html.size());

    bool inside_tag = false;
    bool last_was_space = false;

    for (size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (c == '<') {
            inside_tag = true;
            continue;
        }
        if (c == '>') {
            inside_tag = false;
            if (!last_was_space && !output.empty()) {
                output.push_back(' ');
                last_was_space = true;
            }
            continue;
        }
        if (inside_tag) {
            continue;
        }
        if (c == '&') {
            const size_t semicolon = html.find(';', i + 1);
            if (semicolon != std::string::npos && semicolon - i <= 12) {
                AppendDecodedEntity(html.substr(i + 1, semicolon - i - 1), &output);
                i = semicolon;
                last_was_space = false;
                continue;
            }
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_was_space && !output.empty()) {
                output.push_back(' ');
                last_was_space = true;
            }
            continue;
        }
        output.push_back(c);
        last_was_space = false;
    }

    while (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }
    return output;
}

}  // namespace wqn
