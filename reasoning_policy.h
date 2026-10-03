#pragma once

#include <string>

namespace lb_reasoning {

// Confirmed by the provider's unsupported_value response for this model.
// Keep the exception narrow: an OpenAI-style wire format alone does not
// tell us which reasoning levels another model or provider supports.
inline bool RequiresReasoning(const std::string& model, bool openAIStyle)
{
    return openAIStyle && model == "gpt-6-astra";
}

inline std::string OpenAIEffort(const std::string& model,
                                const std::string& requested)
{
    if (requested == "none" && RequiresReasoning(model, true))
        return "low";
    return requested;
}

} // namespace lb_reasoning
