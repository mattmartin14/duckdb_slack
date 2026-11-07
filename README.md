# DuckDB Slack Extension

The `slack` DuckDB community extension posts messages to a Slack channel using
an incoming webhook stored in DuckDB's native Secrets Manager.

The extension is prepared for a future community submission. It is not
currently published or installable with `INSTALL slack FROM community`.

## Features

- DuckDB's native `slack` secret type, with webhook URLs redacted in secret
  listings.
- Named-secret lookup and temporary or persistent secret storage.
- `slack_post` as a table function callable with `CALL`.
- HTTPS JSON requests, TLS certificate verification, bounded timeouts, and
  no automatic retries.
- A single success or failure status row for each invocation.
- Optional success, error, and informational cards with structured report fields.

The v1 webhook interface does not use a bot token or `chat.postMessage`. Slack
incoming webhooks select their channel when the webhook is created, so the SQL
call does not need a `channel` argument. Arbitrary Block Kit payloads, file
uploads, threaded replies, bulk posting, and scheduled notifications are out
of scope.

## Create a Slack incoming webhook

Create an incoming webhook in your Slack app and associate it with the channel
where messages should be posted. The webhook is bound to that channel. It
cannot be used to select another destination in a `slack_post` call. Keep the
webhook URL private; anyone who obtains it can post to its associated channel.

## Build

This C++ extension uses the DuckDB extension template and targets DuckDB
v2.0.0. Initialize its submodules and install vcpkg (or use an existing
vcpkg installation):

```sh
git submodule update --init --recursive
git clone https://github.com/microsoft/vcpkg.git /path/to/vcpkg
/path/to/vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

The vcpkg manifest installs the extension's HTTP and JSON dependencies. Build
and run the tests with:

```sh
make
make test
```

The build produces a DuckDB shell at `build/release/duckdb` and a loadable
extension at `build/release/extension/slack/slack.duckdb_extension`. Local
builds are unsigned; load them only if you trust the build. For example:

```sh
build/release/duckdb -unsigned
```

Then load the local artifact:

```sql
LOAD './build/release/extension/slack/slack.duckdb_extension';
```

After the extension is accepted and published by the DuckDB Community
Extensions repository, users will be able to install it with:

```sql
INSTALL slack FROM community;
LOAD slack;
```

## Create a secret and post a message

`CREATE SECRET` creates temporary secrets by default:

```sql
CREATE SECRET slack_test (
    TYPE slack,
    WEBHOOK_URL 'https://hooks.slack.com/services/your/workspace/webhook'
);

CALL slack_post(
    secret_name => 'slack_test',
    msg => 'Hello from DuckDB!'
);
```

`secret_name` is the name of an existing DuckDB `slack` secret; it can also
be passed as the first positional argument for compatibility. `msg` is required
and must be passed by name. The destination channel is fixed by the webhook.

### Formatted table notifications

Use `color => 'good'`, `'danger'`, or `'info'` to send a structured Slack
card. Like the reference notifications, these display a title, severity,
UTC timestamp, details, and optional two-column fields. `good` uses a green
accent and success icon, `danger` a red accent and error icon, and `info` a
green accent and blue info icon. Without `color`, the plain-text payload is
unchanged. Set `title` and `fields` only when `color` is supplied.

**Success: completed processing**

```sql
SET VARIABLE report = (
    SELECT map_from_entries(list((metric, count::VARCHAR) ORDER BY idx))
    FROM (VALUES (1, 'Processed', 42), (2, 'Failed', 0)) AS data(idx, metric, count)
);

CALL slack_post(
    secret_name => 'slack_test',
    msg => 'The ETL process completed successfully.',
    color => 'good',
    title => 'Sales ETL Completed',
    fields => getvariable('report')
);
```

**Info: an import is in progress**

```sql
SET VARIABLE import_status = (
    SELECT map_from_entries(list((label, value) ORDER BY idx))
    FROM (VALUES
        (1, 'Source', 'daily_orders.csv'),
        (2, 'Stage', 'Loading'),
        (3, 'Rows loaded', '125000'),
        (4, 'Next step', 'Validate records')
    ) AS data(idx, label, value)
);

CALL slack_post(
    secret_name => 'slack_test',
    msg => 'The daily order import is running.',
    color => 'info',
    title => 'Order Import Update',
    fields => getvariable('import_status')
);
```

**Error: a file failed validation**

```sql
SET VARIABLE validation_error = (
    SELECT map_from_entries(list((label, value) ORDER BY idx))
    FROM (VALUES
        (1, 'Source', 'daily_orders.csv'),
        (2, 'Stage', 'Validation'),
        (3, 'Error type', 'Invalid date'),
        (4, 'Rejected rows', '12'),
        (5, 'Next step', 'Review the rejected rows and retry')
    ) AS data(idx, label, value)
);

CALL slack_post(
    secret_name => 'slack_test',
    msg => 'The daily order import stopped because some order dates were invalid.',
    color => 'danger',
    title => 'Order Import Failed',
    fields => getvariable('validation_error')
);
```

`fields` is a `MAP(VARCHAR, VARCHAR)`; cast numeric values to `VARCHAR` when
building the map. Each entry is a labeled field, not a row in a multi-column
data table. Slack renders two fields side by side, with a maximum of 10 fields
per section; the extension splits larger maps into sections. Field keys must
be non-empty and values non-NULL. Cards support at most 470 fields, and
Slack's other message and Block Kit size limits apply. `msg` and fields use
Slack mrkdwn. The top-level notification uses the supplied title without a
severity prefix, so the details are not displayed twice. Add any desired
label to `title`; severity still appears in the card body. The attachment
fallback includes the details for clients that do not render blocks.

The result contains one row:

| Column | Type | Meaning |
| --- | --- | --- |
| `success` | `BOOLEAN` | Whether Slack accepted the message. |
| `channel` | `VARCHAR` | Empty for incoming webhooks (Slack does not return the channel). |
| `message_ts` | `VARCHAR` | Empty for incoming webhooks (Slack does not return a timestamp). |
| `message` | `VARCHAR` | A concise status or error description. |

## Secret persistence and security

To keep a secret between DuckDB sessions, create it explicitly as persistent:

```sql
CREATE PERSISTENT SECRET slack_test (
    TYPE slack,
    WEBHOOK_URL 'https://hooks.slack.com/services/your/workspace/webhook'
);
```

DuckDB stores persistent secrets in unencrypted binary form under
`~/.duckdb/stored_secrets` by default, protected by filesystem permissions
(mode `0600`). Anyone with access to the DuckDB process account or its files
can access these credentials. Use temporary secrets when persistence is not
needed, and protect persistent-secret directories as carefully as other
credential stores. DuckDB's `secret_directory` setting can change the storage
location.

DuckDB redacts the webhook URL from `duckdb_secrets()` output. The extension
does not include the URL in returned rows or its own error messages. SQL
entered to create a secret can still be visible to the client or shell history,
so avoid entering real webhook URLs in shared sessions. If a webhook URL is
exposed, revoke it in Slack and create a replacement.

## Error behavior and delivery guarantees

- A missing named secret or a secret of another type raises a DuckDB error.
- HTTP errors, rate limits, network failures, oversized responses, and
  malformed success responses return a row with `success = false` and a safe
  explanation.
- HTTP 429 responses include a numeric `Retry-After` value when available. The
  extension does not retry automatically.
- TLS certificate and hostname verification are enabled. Connection and total
  request timeouts are bounded.
- The HTTP request runs only during table-function execution, not while DuckDB
  binds or plans the query. A single scan uses one worker and guards against
  repeated output chunks issuing another request.

This prevents accidental duplicate posts within one normal table-function
scan; it cannot provide exactly-once delivery across query retries, client
retries, cancellation, or a timeout after Slack accepted a request but before
the response arrived. Retrying such an uncertain request can post a duplicate.

## Mock API tests

Automated tests do not contact Slack. The integration test starts a local mock
server and sets `DUCKDB_SLACK_TEST_API_URL`. For safety, the extension accepts
this override only for an HTTP URL whose literal host is `127.0.0.1` or `::1`;
it also requires the exact test-placeholder webhook URL, bypasses proxies, and
does not follow redirects. All other requests use the webhook URL stored in
the secret, which must be an HTTPS `hooks.slack.com/services/` URL. Tests use
fake webhook URLs; never use a real webhook URL with a test server.

## Community submission

`community/description.yml` is a proposed descriptor for a future submission.
Before submission, replace its `repo.ref` with the commit SHA selected for the
submission. It is not a registration or submission to the community repository.
No pull request has been opened.
