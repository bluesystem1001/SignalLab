-- SignalLab stage 4 initial schema.
-- schema_migration is owned by MigrationRunner and is intentionally not here.

CREATE TABLE file_version (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    content_hash TEXT NOT NULL UNIQUE CHECK (
        length(content_hash) = 64
        AND content_hash NOT GLOB '*[^0-9a-f]*'
    ),
    stored_relative_path TEXT NOT NULL UNIQUE,
    original_name TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
    created_utc TEXT NOT NULL
);

CREATE TABLE signal_source (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_type TEXT NOT NULL CHECK (source_type IN ('GENERATED', 'CSV', 'WAV')),
    source_key TEXT NOT NULL UNIQUE,
    source_path TEXT NOT NULL,
    parameters_json TEXT NOT NULL CHECK (length(parameters_json) > 0),
    file_version_id INTEGER,
    created_utc TEXT NOT NULL,
    FOREIGN KEY (file_version_id) REFERENCES file_version(id) ON DELETE SET NULL
);

CREATE TABLE signal_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id INTEGER NOT NULL UNIQUE,
    sample_rate REAL NOT NULL CHECK (sample_rate > 0),
    sample_count INTEGER NOT NULL CHECK (sample_count > 0),
    sample_blob BLOB NOT NULL,
    sample_hash TEXT NOT NULL CHECK (
        length(sample_hash) = 64
        AND sample_hash NOT GLOB '*[^0-9a-f]*'
    ),
    signal_name TEXT NOT NULL,
    signal_unit TEXT NOT NULL,
    signal_source_label TEXT NOT NULL,
    created_utc TEXT NOT NULL,
    FOREIGN KEY (source_id) REFERENCES signal_source(id) ON DELETE CASCADE
);

CREATE TABLE analysis_task (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    signal_record_id INTEGER,
    signal_name TEXT NOT NULL,
    status TEXT NOT NULL CHECK (
        status IN ('PENDING', 'RUNNING', 'SUCCEEDED', 'FAILED', 'INTERRUPTED')
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

CREATE TABLE feature_result (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id INTEGER NOT NULL,
    feature_name TEXT NOT NULL,
    numeric_value REAL,
    value_text TEXT NOT NULL DEFAULT '',
    unit TEXT NOT NULL,
    FOREIGN KEY (task_id) REFERENCES analysis_task(id) ON DELETE CASCADE,
    UNIQUE (task_id, feature_name)
);

CREATE TABLE script_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    script_text TEXT NOT NULL,
    status TEXT NOT NULL,
    created_utc TEXT NOT NULL,
    finished_utc TEXT NOT NULL,
    error_message TEXT NOT NULL DEFAULT ''
);

CREATE TABLE operation_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    entity_id INTEGER,
    detail TEXT NOT NULL DEFAULT '',
    created_utc TEXT NOT NULL
);

CREATE INDEX idx_signal_source_file_version
    ON signal_source(file_version_id);

CREATE INDEX idx_signal_record_sample_hash
    ON signal_record(sample_hash);

CREATE INDEX idx_analysis_task_signal_record
    ON analysis_task(signal_record_id);

CREATE INDEX idx_analysis_task_created_utc
    ON analysis_task(created_utc DESC);

CREATE INDEX idx_feature_result_task
    ON feature_result(task_id);

CREATE INDEX idx_script_history_created_utc
    ON script_history(created_utc DESC);

CREATE INDEX idx_operation_log_created_utc
    ON operation_log(created_utc DESC);
