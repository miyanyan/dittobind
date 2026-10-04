"""Create disposable host configuration fixtures, then launch one C++ process."""
import os
from pathlib import Path
import subprocess
import shutil
import sys
import sysconfig
import tempfile
import venv

executable, scenario = sys.argv[1:]
case = scenario.removesuffix('_options')
with tempfile.TemporaryDirectory(prefix='dittobind-config-') as temporary:
    root = Path(temporary) / '宿主 路径 🐍'
    root.mkdir()
    for folder, value in [('first', 'first'), ('second', 'second'), ('environment', 'environment')]:
        target = root / folder
        target.mkdir()
        (target / 'priority_marker.py').write_text(f"value = {value!r}\n", encoding='utf-8')
    (root / '启动.py').write_text('answer = 42\n', encoding='utf-8')
    (root / '路径模块.py').write_text('answer = 42\n', encoding='utf-8')
    # Pass fixture paths through the native environment, avoiding the Windows
    # narrow main(argc, argv) encoding when the temporary path is non-ASCII.
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith('PYTHON') and key.upper() != '__PYVENV_LAUNCHER__'}
    env.update(DITTO_CONFIG_ROOT=str(root),
               DITTO_CONFIG_BASE_EXECUTABLE=sys._base_executable,
               DITTO_CONFIG_STDLIB=sysconfig.get_path('stdlib'),
               DITTO_CONFIG_DYNLOAD=str(Path(sys.base_prefix) / 'DLLs') if os.name == 'nt'
               else str(Path(sysconfig.get_path('stdlib')) / 'lib-dynload'),
               PYTHONPATH=str(root / 'environment'), PYTHONOPTIMIZE='2',
               PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1',
               PYTHONHOME=sys.base_prefix)
    if case in ('isolated', 'no_environment', 'precedence'):
        env['PYTHONHOME'] = str(root / 'nonexistent-home')
    if case == 'relocated':
        original = Path(executable)
        executable = str(root / ('宿主.exe' if os.name == 'nt' else '宿主'))
        shutil.copy2(original, executable)
        if os.name == 'nt':
            suffix = 't' if sysconfig.get_config_var('Py_GIL_DISABLED') else ''
            dll = f'python{sys.version_info.major}{sys.version_info.minor}{suffix}.dll'
            shutil.copy2(original.parent / dll, root / dll)
        env['DITTO_CONFIG_EXECUTABLE'] = executable
    if case.startswith('venv'):
        environment = root / '虚拟环境'
        venv.EnvBuilder(with_pip=False).create(environment)
        python = environment / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
        clean = {key: value for key, value in env.items() if not key.startswith('PYTHON')}
        site = Path(subprocess.check_output(
            [str(python), '-c', "import sys, sysconfig; sys.stdout.buffer.write(sysconfig.get_path('purelib').encode('utf-8'))"],
            env=clean, encoding='utf-8').strip())
        site.mkdir(parents=True, exist_ok=True)
        (site / 'venv_marker.py').write_text('answer = 42\n', encoding='utf-8')
        env = clean | {'DITTO_CONFIG_VENV': str(environment),
                       'DITTO_CONFIG_EXECUTABLE': str(python)}
        if case == 'venv_switch':
            other = root / '另一个环境'
            venv.EnvBuilder(with_pip=False).create(other)
            other_python = other / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
            other_site = other / site.relative_to(environment)
            other_site.mkdir(parents=True, exist_ok=True)
            (other_site / 'venv_marker.py').write_text('answer = 84\n', encoding='utf-8')
            env.update(DITTO_CONFIG_VENV_B=str(other), DITTO_CONFIG_EXECUTABLE_B=str(other_python))
    result = subprocess.run([executable, scenario], env=env, timeout=25)
    raise SystemExit(result.returncode)
