"""Summarize CTest JUnit reports without treating missing/skipped tests as passes."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('reports', nargs='+', type=Path)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
results = []
for path in args.reports:
    suite = ET.parse(path).getroot()
    groups = {}
    for case in suite.iter('testcase'):
        name = case.attrib['name']
        backend = name.split('.')[0]
        group = groups.setdefault(backend, {'passed': 0, 'failed': [], 'skipped': []})
        if case.find('failure') is not None or case.find('error') is not None:
            group['failed'].append(name)
        elif case.find('skipped') is not None or case.attrib.get('status') == 'notrun':
            group['skipped'].append(name)
        else:
            group['passed'] += 1
    results.append({'report': path.as_posix(), 'timestamp': suite.attrib.get('timestamp'), 'backends': groups})
text = json.dumps(results, indent=2, ensure_ascii=False) + '\n'
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding='utf-8')
print(text, end='')
