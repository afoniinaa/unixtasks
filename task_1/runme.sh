#!/usr/bin/env bash
set -euo pipefail

REPORT_FILE="result.txt"
NEEDED_FILE_SIZE=$((4 * 1024 * 1024 + 1))

print_error_and_exit() {
    echo "ошибка: $1" >&2
    exit 1
}

read_byte_at_position() {
    local file_path="$1"
    local byte_position="$2"
    dd if="${file_path}" bs=1 skip="${byte_position}" count=1 status=none | od -An -tu1 | tr -d '[:space:]'
}

check_files_are_equal() {
    local left_file_path="$1"
    local right_file_path="$2"

    if ! cmp -s "${left_file_path}" "${right_file_path}"; then
        print_error_and_exit "файлы ${left_file_path} и ${right_file_path} отличаются"
    fi
}

if [[ -e "${REPORT_FILE}" && ! -w "${REPORT_FILE}" ]]; then
    rm -f "${REPORT_FILE}" || print_error_and_exit "не могу записать в ${REPORT_FILE}"
fi

exec >"${REPORT_FILE}" 2>&1

echo "проверка разреженного файла"
echo "время запуска: $(date)"
echo

if [[ ! -x ./myprogram ]]; then
    print_error_and_exit "не найден исполняемый файл ./myprogram (сначала соберите проект)"
fi

echo "1) создание тестового файла A"
echo "ожидаем: размер ${NEEDED_FILE_SIZE}, байт 1 на позициях 0, 10000 и в конце"
truncate -s "${NEEDED_FILE_SIZE}" A
printf '\1' | dd of=A bs=1 seek=0 conv=notrunc status=none
printf '\1' | dd of=A bs=1 seek=10000 conv=notrunc status=none
printf '\1' | dd of=A bs=1 seek=$((NEEDED_FILE_SIZE - 1)) conv=notrunc status=none
echo "факт: ОК"

actual_size="$(stat -c '%s' A)"
byte_at_start="$(read_byte_at_position A 0)"
byte_in_middle="$(read_byte_at_position A 10000)"
byte_at_end="$(read_byte_at_position A $((NEEDED_FILE_SIZE - 1)))"

if [[ "${actual_size}" != "${NEEDED_FILE_SIZE}" || "${byte_at_start}" != "1" || "${byte_in_middle}" != "1" || "${byte_at_end}" != "1" ]]; then
    print_error_and_exit "тестовый файл A создан неверно"
fi
echo "факт: ОК, размер=${actual_size}, байты=[${byte_at_start}, ${byte_in_middle}, ${byte_at_end}]"
echo

echo "2) копирование A -> B"
./myprogram A B
check_files_are_equal A B
echo "факт: ОК"
echo

echo "3) сжатие A и B"
gzip -c A >A.gz
gzip -c B >B.gz
if [[ ! -f A.gz || ! -f B.gz ]]; then
    print_error_and_exit "файлы A.gz или B.gz не созданы"
fi
echo "факт: ОК"
echo

echo "4) восстановление из B.gz через pipe в C"
gzip -cd B.gz | ./myprogram C
check_files_are_equal A C
echo "факт: ОК"
echo

echo "5) запуск с -b 100, A -> D"
./myprogram -b 100 A D
check_files_are_equal A D
echo "факт: ОК"
echo

echo "6) статистика файлов"
stat -c '%n: размер=%s байт, блоков=%b, размер_блока=%B' A A.gz B B.gz C D
echo
echo "готово, все обязательные шаги выполнены"
