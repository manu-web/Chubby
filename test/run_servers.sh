#!/bin/bash

pkill -9 -f "chubby_server"
rm -r db_* *.log *.config

# Check if the number of servers (n) is provided as an argument
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <number_of_servers>"
    exit 1
fi

# Get the number of servers to run
n=$1
# Starting port number
start_port=50051

# Loop to run n servers
for ((i=0; i<n; i++)); do
    port=$((start_port + i))
    log_file="${port}.log"
    
    # Run the server in the background and redirect output to the log file
    ./chubby_server 127.0.0.1:$port $n > "$log_file" 2>&1 &
    echo "127.0.0.1:$port" >> $n.config
    echo "Starting server on port $port, logging to $log_file"
done

echo "Wait for all servers to startup"
# Wait for all background jobs to finish
sleep 10
echo "All servers started"