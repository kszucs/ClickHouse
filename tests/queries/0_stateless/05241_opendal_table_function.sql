-- Tags: use-opendal

SELECT * FROM opendal('fs', root = currentDatabase(), path = 'a.csv'); -- { serverError SUPPORT_IS_DISABLED }

SET allow_experimental_opendal_table_function = 1;

INSERT INTO FUNCTION opendal('fs', root = currentDatabase(), path = 'data/a.csv', format = 'CSVWithNames', structure = 'id UInt32, name String')
SELECT number + 1, ['alice', 'bob'][number + 1] FROM numbers(2);
INSERT INTO FUNCTION opendal('fs', root = currentDatabase(), path = 'data/sub/b.csv', format = 'CSVWithNames', structure = 'id UInt32, name String')
SELECT 3, 'carol';

SELECT * FROM opendal('fs', root = currentDatabase(), path = 'data/a.csv', format = 'CSVWithNames') ORDER BY id;
SELECT _file, id, name FROM opendal('fs', root = currentDatabase(), path = 'data/**/*.csv', format = 'CSVWithNames') ORDER BY id;
SELECT id FROM opendal('fs', root = currentDatabase(), path = 'data/*.csv', format = 'CSVWithNames', structure = 'id UInt32, name String') ORDER BY id;

SELECT * FROM opendal('fs', root = currentDatabase(), path = 'data/missing.csv', format = 'CSVWithNames', structure = 'id UInt32'); -- { serverError FILE_DOESNT_EXIST }
SELECT * FROM opendal('memory', path = 'a.csv'); -- { serverError BAD_ARGUMENTS }
SELECT * FROM opendal('fs', root = currentDatabase()); -- { serverError BAD_ARGUMENTS }
SELECT * FROM opendal('fs', 'a.csv'); -- { serverError BAD_ARGUMENTS }

-- A server only reads under `user_files_path`.
SELECT * FROM opendal('fs', root = '/etc', path = 'passwd', format = 'LineAsString'); -- { serverError DATABASE_ACCESS_DENIED }
SELECT * FROM opendal('fs', root = '../..', path = 'etc/passwd', format = 'LineAsString'); -- { serverError DATABASE_ACCESS_DENIED }
SELECT * FROM opendal('fs', root = currentDatabase(), path = '../../../../etc/passwd', format = 'LineAsString'); -- { serverError FILE_DOESNT_EXIST }
SELECT * FROM opendal('fs', root = currentDatabase(), path = '/etc/passwd', format = 'LineAsString'); -- { serverError FILE_DOESNT_EXIST }

-- Options other than the known plain ones are hidden in logs.
SELECT count() FROM opendal('fs', root = currentDatabase(), path = 'data/a.csv', format = 'CSVWithNames', token = 'opendal_secret_value');
SYSTEM FLUSH LOGS query_log;
SELECT count() FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND query LIKE '%opendal\_secret\_value%';
SELECT count() > 0 FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND query LIKE '%token = \'[HIDDEN]\'%';
