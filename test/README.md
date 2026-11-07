# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most of its tests in this format as SQL statements, so for the Slack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or
```bash
make test_debug
```

The webhook HTTP integration tests are run by the root `make test` target and
use a loopback-only mock server. They do not contact Slack. They verify the
outgoing plain-text payload and formatted table cards with `good` and `danger`
colors, in addition to delivery failures and secret handling.