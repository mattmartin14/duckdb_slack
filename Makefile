PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=slack_msg
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_slack_integration
ifneq ($(filter test,$(MAKECMDGOALS)),)
test_slack_integration: release
endif
test_release: test_slack_integration
test_slack_integration:
	@if [ "$(SKIP_TESTS)" = "1" ]; then \
		echo "Slack integration tests are skipped in this run..."; \
	else \
		python3 test/integration/slack_webhook_test.py; \
	fi