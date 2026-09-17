-- Stage 5 adds an explicit terminal state for user-requested cancellation.
-- SQLite cannot replace a CHECK constraint in place, so both the parent task
-- table and its feature child table are rebuilt inside MigrationRunner's
-- transaction. Existing ids, task history and feature rows are preserved.

CREATE TABLE analysis_task_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    signal_record_id INTEGER,
    signal_name TEXT NOT NULL,
    status TEXT NOT NULL CHECK (
        status IN (
            'PENDING',
            'RUNNING',
            'SUCCEEDED',
            'FAILED',
            'INTERRUPTED',
            'CANCELLED'
        )
    ),
    algorithm_name TEXT NOT NULL,
    algorithm_version TEXT NOT NULL,
    parameters_json TEXT NOT NULL CHECK (length(parameters_json) > 0),
    input_hash TEXT NOT NULL CHECK (
        length(input_hash) = 64
        AND input_hash NOT GLOB '*[^0-9a-f]*'
    ),
    output_hash TEXT NOT NULL DEFAULT '' CHECK (
        output_hash = '' OR (
            length(output_hash) = 64
            AND output_hash NOT GLOB '*[^0-9a-f]*'
        )
    ),
    created_utc TEXT NOT NULL,
    started_utc TEXT NOT NULL,
    finished_utc TEXT NOT NULL DEFAULT '',
    error_message TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (signal_record_id) REFERENCES signal_record(id) ON DELETE SET NULL
);

INSERT INTO analysis_task_new (
    id,
    signal_record_id,
    signal_name,
    status,
    algorithm_name,
    algorithm_version,
    parameters_json,
    input_hash,
    output_hash,
    created_utc,
    started_utc,
    finished_utc,
    error_message
)
SELECT
    id,
    signal_record_id,
    signal_name,
    status,
    algorithm_name,
    algorithm_version,
    parameters_json,
    input_hash,
    output_hash,
    created_utc,
    started_utc,
    finished_utc,
    error_message
FROM analysis_task;

CREATE TABLE feature_result_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id INTEGER NOT NULL,
    feature_name TEXT NOT NULL,
    numeric_value REAL,
    value_text TEXT NOT NULL DEFAULT '',
    unit TEXT NOT NULL,
    FOREIGN KEY (task_id) REFERENCES analysis_task_new(id) ON DELETE CASCADE,
    UNIQUE (task_id, feature_name)
);

INSERT INTO feature_result_new (
    id,
    task_id,
    feature_name,
    numeric_value,
    value_text,
    unit
)
SELECT
    id,
    task_id,
    feature_name,
    numeric_value,
    value_text,
    unit
FROM feature_result;

DROP TABLE feature_result;
DROP TABLE analysis_task;

ALTER TABLE analysis_task_new RENAME TO analysis_task;
ALTER TABLE feature_result_new RENAME TO feature_result;

CREATE INDEX idx_analysis_task_signal_record
    ON analysis_task(signal_record_id);

CREATE INDEX idx_analysis_task_created_utc
    ON analysis_task(created_utc DESC);

CREATE INDEX idx_feature_result_task
    ON feature_result(task_id);
