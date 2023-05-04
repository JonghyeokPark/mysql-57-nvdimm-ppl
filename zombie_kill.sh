read -p "Do you want to kill zombie? (Y/N)" answer
if [[ $answer == "y" || $answer == "Y" ]]; then
    ps -ef | grep defunct | awk '{print $3}' | xargs kill -9 # kill all zombie
fi
