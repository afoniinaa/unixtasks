#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_READ_BLOCK_SIZE 4096

static void print_system_error_and_exit(const char *message) {
    fprintf(stderr, "%s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [-b block_size] output_file\n"
            "  %s [-b block_size] input_file output_file\n",
            program_name, program_name);
}

static size_t parse_block_size_or_exit(const char *text_value) {
    char *parse_end = NULL;
    unsigned long long parsed_value = 0;

    errno = 0;
    parsed_value = strtoull(text_value, &parse_end, 10);
    if (errno != 0 || parse_end == text_value || *parse_end != '\0' || parsed_value == 0) {
        fprintf(stderr, "Invalid block size: %s\n", text_value);
        exit(EXIT_FAILURE);
    }
    if (parsed_value > SIZE_MAX) {
        fprintf(stderr, "Block size is too large: %s\n", text_value);
        exit(EXIT_FAILURE);
    }

    return (size_t)parsed_value;
}

static int is_all_zero_bytes(const unsigned char *buffer, size_t bytes_to_check) {
    size_t byte_index = 0;

    while (byte_index < bytes_to_check) {
        if (buffer[byte_index] != 0) {
            return 0;
        }
        byte_index++;
    }

    return 1;
}

static void write_all_bytes_or_exit(int output_file_descriptor, const unsigned char *buffer, size_t bytes_to_write) {
    size_t written_bytes = 0;

    while (written_bytes < bytes_to_write) {
        ssize_t current_write_result = write(
            output_file_descriptor,
            buffer + written_bytes,
            bytes_to_write - written_bytes
        );

        if (current_write_result < 0) {
            print_system_error_and_exit("write() failed");
        }

        written_bytes += (size_t)current_write_result;
    }
}

int main(int argument_count, char **argument_values) {
    size_t read_block_size = DEFAULT_READ_BLOCK_SIZE;
    const char *input_file_path = NULL;
    const char *output_file_path = NULL;
    int input_file_descriptor = STDIN_FILENO;
    int output_file_descriptor = -1;
    unsigned char *read_buffer = NULL;
    off_t output_logical_size = 0;
    int option = 0;

    while ((option = getopt(argument_count, argument_values, "b:")) != -1) {
        if (option != 'b') {
            print_usage(argument_values[0]);
            return EXIT_FAILURE;
        }

        read_block_size = parse_block_size_or_exit(optarg);
    }

    if (argument_count - optind == 1) {
        output_file_path = argument_values[optind];
    } else if (argument_count - optind == 2) {
        input_file_path = argument_values[optind];
        output_file_path = argument_values[optind + 1];
    } else {
        print_usage(argument_values[0]);
        return EXIT_FAILURE;
    }

    if (input_file_path != NULL) {
        input_file_descriptor = open(input_file_path, O_RDONLY);
        if (input_file_descriptor < 0) {
            print_system_error_and_exit("open(input) failed");
        }
    }

    output_file_descriptor = open(output_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_file_descriptor < 0) {
        print_system_error_and_exit("open(output) failed");
    }

    read_buffer = malloc(read_block_size);
    if (read_buffer == NULL) {
        print_system_error_and_exit("malloc() failed");
    }

    while (1) {
        ssize_t read_result = read(input_file_descriptor, read_buffer, read_block_size);
        if (read_result < 0) {
            print_system_error_and_exit("read() failed");
        }
        if (read_result == 0) {
            break;
        }

        {
            size_t bytes_read = (size_t)read_result;
            output_logical_size += (off_t)bytes_read;

            if (is_all_zero_bytes(read_buffer, bytes_read)) {
                if (lseek(output_file_descriptor, (off_t)bytes_read, SEEK_CUR) == (off_t)-1) {
                    print_system_error_and_exit("lseek() failed");
                }
            } else {
                write_all_bytes_or_exit(output_file_descriptor, read_buffer, bytes_read);
            }
        }
    }

    if (ftruncate(output_file_descriptor, output_logical_size) != 0) {
        print_system_error_and_exit("ftruncate() failed");
    }

    free(read_buffer);

    if (input_file_path != NULL && close(input_file_descriptor) != 0) {
        print_system_error_and_exit("close(input) failed");
    }
    if (close(output_file_descriptor) != 0) {
        print_system_error_and_exit("close(output) failed");
    }

    return EXIT_SUCCESS;
}
