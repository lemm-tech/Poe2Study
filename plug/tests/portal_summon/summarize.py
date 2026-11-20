"""Summarize first appearances and retirements from an independent portal probe."""
import argparse
import datetime as dt
import json
from pathlib import Path


def identity(portal):
    return portal['key'], portal['entity']


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--reported-clicks', type=int, required=True)
    args = ap.parse_args()
    directory = args.input.resolve()
    session = json.loads((directory / 'session.json').read_text(encoding='utf-8'))
    baseline = json.loads((directory / 'baseline.json').read_text(encoding='utf-8'))
    events = [baseline] + [json.loads(p.read_text(encoding='utf-8'))
                           for p in sorted(directory.glob('change_*.json'))]
    known = {identity(p) for p in baseline['portals']}
    previous = {identity(p): p for p in baseline['portals']}
    matched, other, retirements = [], [], []
    last_snapshot = baseline
    for event in events[1:]:
        current = {identity(p): p for p in event['portals']}
        for ident, portal in current.items():
            if ident in known:
                continue
            known.add(ident)
            row = {'firstSeenAt': event['capturedAt'], 'sourceChangeIndex': event['changeIndex'],
                   'sourceFile': f"change_{event['changeIndex']:03d}.json",
                   'playerPosition': event['player']['position'], **portal}
            if portal['position']:
                row['offsetFromPlayer'] = [a - b for a, b in zip(portal['position'], event['player']['position'])]
            qualifies = portal['portalMetadata'] and portal['hideoutName'] and portal['nearPlayer']
            (matched if qualifies else other).append(row)
        for ident in previous.keys() - current.keys():
            retirements.append({'firstAbsentAt': event['capturedAt'], 'key': ident[0], 'entity': ident[1],
                                'sourceChangeIndex': event['changeIndex']})
        previous, last_snapshot = current, event
    for index, row in enumerate(matched, 1):
        row['appearanceNumber'] = index
        gone = next((r for r in retirements if (r['key'], r['entity']) == identity(row)), None)
        row['firstAbsentAt'] = gone['firstAbsentAt'] if gone else None
        if gone:
            overlapping = next((r for r in matched if r['firstSeenAt'] > row['firstSeenAt'] and
                                r['firstSeenAt'] <= gone['firstAbsentAt']), None)
            if overlapping:
                row['secondsAfterReplacementUntilAbsent'] = round((
                    dt.datetime.fromisoformat(gone['firstAbsentAt']) -
                    dt.datetime.fromisoformat(overlapping['firstSeenAt'])).total_seconds(), 3)
    observations = []
    for line in (directory / 'observations.jsonl').read_text(encoding='utf-8').splitlines():
        if line.strip():
            observations.append(json.loads(line))
    ids = sorted({r['currentSkillId'] for r in observations if r.get('currentSkillId') is not None})
    ended_path = directory / 'ended.json'
    ended = json.loads(ended_path.read_text(encoding='utf-8')) if ended_path.exists() else None
    report = {'createdAt': dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(),
              'scope': 'Independent read-only polling; no production integration, game calls, writes or packets.',
              'session': session, 'ended': ended, 'sourceDirectory': str(directory),
              'reportedClicks': args.reported_clicks, 'qualifyingFirstAppearances': len(matched),
              'portalChangeEvents': len(events) - 1, 'matches': matched,
              'otherFirstAppearances': other, 'retirements': retirements,
              'observedCurrentSkillIds': [{'decimal': value, 'hex': hex(value)} for value in ids],
              'readErrorEvents': sum(r.get('type') == 'read_error' for r in observations),
              'lastEventAt': last_snapshot['capturedAt'],
              'findings': {'allMatchingPortalsSameName': len({r['name'] for r in matched}) == 1,
                           'allMatchingOffsetsPlus5Plus5': bool(matched) and all(r.get('offsetFromPlayer') == [5, 5] for r in matched),
                           'allMatchingPlayerPositionsDistinct': len({tuple(r['playerPosition']) for r in matched}) == len(matched)},
              'limits': ['First appearance times are observation times, not exact button-click times.',
                         'The +5,+5 offset is observed in this session, not a proven universal spawn rule.',
                         'The 120-pixel radius is an observation filter, not an established game limit.',
                         'Polling does not establish the summon function, skill ID or packet payload.']}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'reportedClicks': report['reportedClicks'],
                      'qualifyingFirstAppearances': len(matched), 'portalChangeEvents': len(events) - 1,
                      'findings': report['findings'], 'ended': ended,
                      'observedCurrentSkillIds': report['observedCurrentSkillIds'],
                      'matches': [{k: r[k] for k in ['appearanceNumber', 'firstSeenAt', 'key', 'entity',
                                                   'playerPosition', 'position', 'offsetFromPlayer', 'distance',
                                                   'firstAbsentAt']}
                                  for r in matched],
                      'other': [{k: r[k] for k in ['key', 'position', 'distance']} for r in other]},
                     ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
