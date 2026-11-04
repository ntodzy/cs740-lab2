#!/usr/bin/env bash
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"
python_bin=${PYTHON:-python3}

usage() {
    cat <<'EOF'
Usage: bash evaluation/run.sh <command> [arguments]
  init                  Check Python 3.9+, build the simulator; no pip packages needed.
  list                  List selectable regression suites.
  test <suite> [...]    Run selected suites (or 'all').
  evaluate [options]    Sweep paired ECMP/CONGA loads; see evaluate --help.
  aggregate [options]   Rebuild aggregate CSV/plots from raw structured output.

Examples:
  bash evaluation/run.sh init
  bash evaluation/run.sh test output conga evaluation
  bash evaluation/run.sh evaluate --distribution uniform --small --seeds 1729 12314
  bash evaluation/run.sh evaluate --distribution all --seeds 1729 12314 --jobs 2 --output logs/final
EOF
}

command_name=${1:-help}
if (($#)); then shift; fi
case "$command_name" in
    init)
        "$python_bin" -c 'import sys; assert sys.version_info >= (3, 9), "Python 3.9+ required"'
        make -C htsim
        printf 'Ready: no third-party Python packages required. Run "bash evaluation/run.sh list".\n'
        ;;
    list)
        printf '%s\n' conga routing ecmp dre leaf metadata events output evaluation all
        ;;
    evaluate|aggregate)
        python_command=run
        if [[ "$command_name" == aggregate ]]; then python_command=aggregate; fi
        exec "$python_bin" evaluation/evaluate.py "$python_command" "$@"
        ;;
    test)
        if (($# == 0)); then usage; exit 2; fi
        suites=("$@")
        if [[ " $* " == *' all '* ]]; then
            suites=(conga routing ecmp dre leaf metadata events output evaluation)
        fi
        # Validate the entire selection before executing anything.
        for suite in "${suites[@]}"; do
            case "$suite" in conga|routing|ecmp|dre|leaf|metadata|events|output|evaluation) ;;
                *) printf 'Unknown test suite: %s\n' "$suite" >&2; exit 2 ;;
            esac
        done
        make -C htsim
        for suite in "${suites[@]}"; do
            printf '\nRunning %s\n' "$suite"
            case "$suite" in
                conga) bash htsim/tests/test-conga.sh ;;
                routing) bash htsim/tests/test-routing-randomness.sh ;;
                ecmp) node htsim/tests/test-ecmp.mjs ;;
                dre) bash htsim/tests/test-dre.sh ;;
                leaf) bash htsim/tests/test-leaf-feedback.sh ;;
                metadata) bash htsim/tests/test-conga-metadata.sh ;;
                events) bash htsim/tests/test-eventlist.sh ;;
                output) node htsim/tests/test-output.mjs ;;
                evaluation) "$python_bin" -B -m unittest discover -s evaluation/tests -v ;;
            esac
        done
        ;;
    help|--help|-h) usage ;;
    *) usage >&2; exit 2 ;;
esac
