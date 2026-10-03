import json
import os
import platform
import sys

data = {
    'ok': True,
    'helper': 'python_health',
    'python_version': sys.version.split()[0],
    'python_executable': sys.executable,
    'cwd': os.getcwd(),
    'platform': platform.platform(),
}
print(json.dumps(data, indent=2))
