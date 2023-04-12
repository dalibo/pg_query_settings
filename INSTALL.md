```
git clone XXX
cd pg_query_settings
make
sudo make install
```

If you have multiple versions of PostgreSQL on the server, you may need to
specify which version is your target by setting the PG_CONFIG env variable.

For example :

```
PG_CONFIG=/usr/lib/postgresql/14/bin/pg_config
export PG_CONFIG
```
