# Commands used to generate keys

## Generate rootCA

```
openssl genrsa -out rootCA.key 4096 
openssl req -x509 -new -nodes -key rootCA.key -sha256 -days 1024 -out rootCA.crt
```

# Generate keys and certs for server

```
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr      
openssl x509 -req -in server.csr -CA rootCA.crt -CAkey rootCA.key -CAcreateserial -out server.crt -days 500 -sha256
```

# Generate keys and certs for client

```
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr      
openssl x509 -req -in client.csr -CA rootCA.crt -CAkey rootCA.key -CAcreateserial -out client.crt -days 500 -sha256
```
