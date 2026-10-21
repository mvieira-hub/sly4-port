#!/bin/sh
# Waits for the latest GitHub Actions run of sly4-port and prints its jobs and test lines.
# Runs in an alpine container with the gh login volume:
#   docker run --rm -v ferramentas-gh:/root/.config/gh -v "${PWD}\tools:/t:ro" alpine sh /t/ci_watch.sh
REPO=mvieira-hub/sly4-port
apk add -q github-cli >/dev/null 2>&1
sleep 10
id=$(gh run list -R "$REPO" --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$id" -R "$REPO" --exit-status --interval 30 >/dev/null 2>&1
status=$?
gh run view "$id" -R "$REPO" | sed -n '1,8p'
gh run view "$id" -R "$REPO" --log 2>/dev/null | grep -E '^tests.*(ok |pulado|FALHOU|falharam)' | sed 's/.*Z //'
exit $status
