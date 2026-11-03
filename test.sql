CREATE SECRET slack_test (
    TYPE slack,
    WEBHOOK_URL 'https://hooks.slack.com/services/your/workspace/webhook'
);

CALL slack_post(
    'slack_test',
    msg => 'Hello from DuckDB!'
);