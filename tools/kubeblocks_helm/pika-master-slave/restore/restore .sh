set -o pipefail

function handle_exit() {
    exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "failed with exit code $exit_code"

        touch "${DP_BACKUP_INFO_FILE}.exit"

        exit 1
    fi
}
trap handle_exit EXIT

export PATH="$PATH:$DP_DATASAFED_BIN_PATH"

export DATASAFED_BACKEND_BASE_PATH="$DP_BACKUP_BASE_PATH"

connect_url="redis-cli -h ${DP_DB_HOST} -p ${DP_DB_PORT} -a ${DP_DB_PASSWORD}"

last_save=$(${connect_url} LASTSAVE)

echo "INFO: start BGSAVE"
${connect_url} BGSAVE

echo "INFO: wait for saving rdb successfully"
while true; do
  end_save=$(${connect_url} LASTSAVE)

  if [ $end_save -ne $last_save ];then
     break

  fi
  sleep 1

done

echo "INFO: start to save data file..."
cd ${DATA_DIR}

if [ "${MODE}" == "cluster" ]; then

  datasafed push -z zstd-fastest ./dump.rdb "${DP_BACKUP_NAME}.rdb.zst"

  datasafed push ./Pika.conf "Pika.conf"

  export DATASAFED_BACKEND_BASE_PATH="$(dirname $DP_BACKUP_BASE_PATH)"

else

  tar -cvf - ./ | datasafed push -z zstd-fastest - "${DP_BACKUP_NAME}.tar.zst"

fi

echo "INFO: save data file successfully"

TOTAL_SIZE=$(datasafed stat / | grep TotalSize | awk '{print $2}')

echo "{\"totalSize\":\"$TOTAL_SIZE\"}" >"${DP_BACKUP_INFO_FILE}" && sync
