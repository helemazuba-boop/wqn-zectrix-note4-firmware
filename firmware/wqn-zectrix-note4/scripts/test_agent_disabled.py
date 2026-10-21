#!/usr/bin/env python3
"""Link every public Agent entry point against the production disabled stubs.

The model and ESP error types are host ports. Full ESP-IDF feature-off builds
verify the real preprocessor path; this is not a network or device test.
"""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    header = (root / 'main/opencode_session.h').read_text()
    source = (root / 'main/opencode_session.cpp').read_text()
    branch = source[source.rindex('\n#else'):source.rindex('\n#endif')]
    branch = branch[branch.index('namespace wqn {'):]
    names = re.findall(r'^(?:esp_err_t|bool|void) (\w+)\(', header, re.M)
    assert names and len(names) == len(set(names))
    # Volatile address references force the linker to resolve every actual
    # declaration, including entry points whose return values are not checked.
    references = '\n'.join(
        f'    auto* volatile entry_{i} = &wqn::{name}; (void)entry_{i};'
        for i, name in enumerate(names))
    unit = '#include "opencode_session.h"\n#include <cassert>\n' + branch + '''
namespace wqn { struct AgentSessionState { int sentinel = 42; }; }
int main() {
''' + references + '''
    assert(wqn::InitOpenCodeSession() == ESP_ERR_NOT_SUPPORTED);
    assert(wqn::ReplyPendingOpenCodeQuestion(0) == ESP_ERR_NOT_SUPPORTED);
    wqn::LeaveOpenCodeAgentTier();
    wqn::InterruptOpenCodeRun();
    wqn::AgentSessionState state;
    assert(!wqn::CopyOpenCodeSessionToUi(&state) && state.sentinel == 42);
    assert(!wqn::ScrollOpenCodeResponse(1, -10, 0, 1));
}
'''
    with tempfile.TemporaryDirectory(prefix='wqn-agent-disabled-') as temp:
        folder = Path(temp)
        (folder / 'opencode_session.h').write_text(header)
        (folder / 'opencode_model.h').write_text(
            '#pragma once\n#include <cstdint>\n#include <string>\n'
            'namespace wqn { struct AgentSessionState; }\n')
        (folder / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t = int;\n'
            'constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;\n')
        cpp, binary = folder / 'disabled.cpp', folder / 'disabled'
        cpp.write_text(unit)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(cpp), '-o', str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=10)
    print(f'Agent disabled: {len(names)} public entry points linked; refusals and no-ops PASS')


if __name__ == '__main__':
    main()
