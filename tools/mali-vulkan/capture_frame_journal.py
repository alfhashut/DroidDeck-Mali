"""Capture an explicitly armed journal from the current app's guest session."""
import argparse
import json
from steam_frame_journal import finish, schedule


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, choices=range(1, 28), default=10)
    args = parser.parse_args()
    emit = lambda label, value: print(label + '=' + json.dumps(value, sort_keys=True), flush=True)
    config = schedule(args.seconds, emit)
    finish(config, emit)
    return 0 if config else 1


if __name__ == '__main__':
    raise SystemExit(main())
