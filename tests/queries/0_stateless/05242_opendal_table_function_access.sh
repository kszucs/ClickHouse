#!/usr/bin/env bash
# Tags: use-opendal

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

user="user_${CLICKHOUSE_DATABASE}"
collection="collection_${CLICKHOUSE_DATABASE}"
settings="SETTINGS allow_experimental_opendal_table_function = 1"
function opendal()
{
    echo "opendal('fs', root = '${CLICKHOUSE_DATABASE}', path = '$1', format = 'CSVWithNames', structure = 'id UInt32')"
}

${CLICKHOUSE_CLIENT} --query "INSERT INTO FUNCTION $(opendal a.csv) SELECT 1 ${settings}"
${CLICKHOUSE_CLIENT} --query "DROP USER IF EXISTS ${user}; CREATE USER ${user} IDENTIFIED WITH no_password; GRANT CREATE TEMPORARY TABLE ON *.* TO ${user}"

echo "--- no grant"
${CLICKHOUSE_CLIENT} --user "${user}" --query "SELECT count() FROM $(opendal a.csv) ${settings}" 2>&1 | grep -o -m1 'READ ON OPENDAL'

echo "--- READ ON OPENDAL"
${CLICKHOUSE_CLIENT} --query "GRANT READ ON OPENDAL TO ${user}"
${CLICKHOUSE_CLIENT} --user "${user}" --query "SELECT count() FROM $(opendal a.csv) ${settings}"
${CLICKHOUSE_CLIENT} --user "${user}" --query "INSERT INTO FUNCTION $(opendal b.csv) SELECT 1 ${settings}" 2>&1 | grep -o -m1 'WRITE ON OPENDAL'

echo "--- named collection"
${CLICKHOUSE_CLIENT} --query "CREATE NAMED COLLECTION ${collection} AS scheme = 'fs', root = '${CLICKHOUSE_DATABASE}', format = 'CSVWithNames', structure = 'id UInt32'"
${CLICKHOUSE_CLIENT} --query "SELECT count() FROM opendal(${collection}, path = 'a.csv') ${settings}"

${CLICKHOUSE_CLIENT} --query "DROP NAMED COLLECTION ${collection}; DROP USER ${user}"
