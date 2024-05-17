 helm uninstall pika && helm uninstall pika-cluster
 helm install pika ./pika && helm install pika-cluster ./pika-cluster

sleep 10

helm template pika ./pika --output-dir ./output/pika
helm template pika-cluster ./pika-cluster --output-dir ./output/pika-cluster