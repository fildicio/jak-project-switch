#!/bin/bash
cd "$(dirname "$0")/build-host/game"
exec ./gk --game jak2 -boot -fakeiso
