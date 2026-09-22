-- Stage 6 gives script executions a persistent lifecycle and source identity.
-- Empty source_hash is reserved for rows written before this migration. This
-- keeps every legacy value intact while applying strict constraints to all
-- newly created lifecycle records.

ALTER TABLE script_history
ADD COLUMN source_hash TEXT NOT NULL DEFAULT '' CHECK (
    source_hash = '' OR (
        length(source_hash) = 64
        AND source_hash NOT GLOB '*[^0-9a-f]*'
        AND status IN (
            'RUNNING',
            'SUCCEEDED',
            'FAILED',
            'CANCELLED',
            'INTERRUPTED'
        )
        AND (
            (status = 'RUNNING' AND finished_utc = '')
            OR (status <> 'RUNNING' AND length(finished_utc) > 0)
        )
    )
);

ALTER TABLE script_history
ADD COLUMN error_line INTEGER;

ALTER TABLE script_history
ADD COLUMN error_column INTEGER CHECK (
    (
        (error_line IS NULL AND error_column IS NULL)
        OR (
            error_line IS NOT NULL
            AND error_column IS NOT NULL
            AND typeof(error_line) = 'integer'
            AND typeof(error_column) = 'integer'
            AND error_line >= 1
            AND error_column >= 1
        )
    )
    AND (
        source_hash = ''
        OR (
            status IN ('RUNNING', 'SUCCEEDED')
            AND error_line IS NULL
            AND error_column IS NULL
            AND error_message = ''
        )
        OR (
            status IN ('FAILED', 'CANCELLED', 'INTERRUPTED')
            AND length(error_message) > 0
        )
    )
);
