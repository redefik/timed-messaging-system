#!/bin/bash

# Compile the module with TEST defined
# Run this script after sudoing in your shell

module="../timed-msg-system.ko"
major_parameter="/sys/module/timed_msg_system/parameters/major" # Exported by the module for testing purpose
max_msg_size="/sys/module/timed_msg_system/parameters/max_message_size"
max_storage_size="/sys/module/timed_msg_system/parameters/max_storage_size"
test_file="/dev/my_test_dev"

echo "******************************Test Case no.1*********************************"
echo "Expected: cat shows pippo pluto and returns: Resource Temporarily Unavailable"
echo "Output:"
insmod $module
major=$(<$major_parameter)
mknod $test_file c $major 0
echo "pippo" > $test_file
echo "pluto" > $test_file
cat $test_file # RTU because the default is non-blocking read (socket-like)
rm -f /dev/my_test_dev
rmmod $module

echo "******************************Test Case no.2*********************************"
echo "Expected: echo returns: Message too long"
echo "Output:"
insmod $module
major=$(<$major_parameter)
mknod $test_file c $major 0
echo 1 > $max_msg_size
echo "pippo" > $test_file
rm -f /dev/my_test_dev
rmmod $module

echo "******************************Test Case no.3*********************************"
echo "Expected: no error"
echo "Output:"
insmod $module
major=$(<$major_parameter)
mknod $test_file c $major 0
echo "pippo" > $test_file
echo "pluto" > $test_file
rm -f /dev/my_test_dev
rmmod $module

echo "******************************Test Case no.4*********************************"
echo "Expected: echo fails with: Resource Temporary Unavailable and ab is printed"
echo "Output:"
insmod $module
major=$(<$major_parameter)
mknod $test_file c $major 0
echo 3 > $max_storage_size
echo "ab" > $test_file
echo "pippo" > $test_file # RTU because the device file is already full
cat $test_file
rm -f /dev/my_test_dev
rmmod $module
