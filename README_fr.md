README (FR)
===========

`pg_query_settings` est un module qui permet de modifier un ou plusieurs
paramètres, comme le `work_mem`, à la volée pour certaines requêtes.

Prérequis
----------

Pour compiler ce module, nous avons besoin des fichiers d'en-tête de PostgreSQL
14+ et de l'outil `pg_config`. Les fichiers d'en-tête et cet outil sont
généralement disponibles dans un paquet -dev.

Compilation
-----------

Il suffit d'exécuter :

```
$ make
$ make install
```

Utilisation
-----------

L'extension doit être créée dans un premier temps :

```sql
CREATE EXTENSION pg_query_settings;
```

puis il faut charger la bibliothèque soit avec `LOAD` :

```sql
LOAD 'pg_query_settings';
```

soit avec les paramètres habituels (`shared_preload_libraries` par exemple).

À chaque exécution de requête, la bibliothèque va lire la table `pgqs_config` à
la recherche du queryid de la requête. Pour chaque ligne de `pgqs_config` ayant
ce queryid, la deuxième colonne indique le nom du paramètre et la troisième
colonne la valeur de ce paramètre.

La table `config` se remplit avec des requêtes standards (`INSERT`, `UPDATE`,
`DELETE`).

L'exécution de la fonction de la bibliothèque est automatique une fois qu'elle
est chargée.

Il est possible de désactiver l'extension avec le paramètre
`pg_query_settings.enabled`.

Plus d'informations sur pg_query_settings
-----------------------------------------

Création de l'extension :

```
postgres=# CREATE EXTENSION pg_query_settings;
CREATE EXTENSION
Time: 26.724 ms
```

Création et peuplement d'une table utilisateur :

```
postgres=# CREATE TABLE toto (c1 integer, c2 text);
CREATE TABLE
Time: 22.244 ms
postgres=# INSERT INTO toto SELECT i, 'Ligne '||i FROM generate_series(1, 10000000) i;
INSERT 0 10000000
Time: 21240.040 ms (00:21.240)
```

Afin de récupérer l'identifiant d'une requête, il sera probablement nécessaire
d'activer dans la session le paramètre `compute_query_id` :

```
postgres=# SET compute_query_id TO on;
SET
```

Exécution d'une requête qui génère un tri :

```
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=28686.665..41124.312 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: external merge  Disk: 272920kB                                      │
│   ->  Seq Scan on public.toto (actual time=0.029..12849.197 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1'                                                 │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.125 ms                                                            │
│ Execution Time: 52424.680 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)

Time: 52425.401 ms (00:52.425)
```

Cette requête fait un tri sur disque faute de suffisamment de `work_mem`.

On augmente la `work_mem` dans la session :

```
postgres=# SET work_mem TO '1GB';
SET
Time: 0.624 ms
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=31700.626..43454.941 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: quicksort  Memory: 1020995kB                                        │
│   ->  Seq Scan on public.toto (actual time=0.026..13009.674 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1', work_mem = '1GB'                               │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.376 ms                                                            │
│ Execution Time: 55004.212 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)
Time: 55006.149 ms (00:55.006)
```

On voit bien que le tri est cette fois réalisé en mémoire même si la durée
est sensiblement la même.

On revient à la configuration par défaut (4 Mo) :

```
postgres=# RESET work_mem;
RESET
Time: 0.527 ms
```

On insère la configuration à appliquer dans la table `pgqs_config` en récupérant
le `queryid` sur le plan d'exécution (ligne `Query Identifier`) :

```
postgres=# INSERT INTO pgqs_config VALUES (2507635424379213761, 'work_mem', '1000000000');
INSERT 0 1
Time: 11.757 ms
```

On rejoue la requête... :

```
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=29046.787..41640.526 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: external merge  Disk: 272920kB                                      │
│   ->  Seq Scan on public.toto (actual time=0.035..12971.535 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1'                                                 │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.162 ms                                                            │
│ Execution Time: 53110.755 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)

Time: 53111.479 ms (00:53.111)
```

On ne voit pas notre configuration appliquée...  normal, on n'a pas chargé la
bibliothèque !

On charge la bibliothèque :

```
postgres=# LOAD 'pg_query_settings';
LOAD
Time: 1.493 ms
```

On ré-exécute la requête... :

```
postgres=# EXPLAIN (COSTS OFF, ANALYZE, SETTINGS, VERBOSE) SELECT * FROM toto ORDER BY c2;
WARNING:  queryid is '2507635424379213761'
WARNING:  value is 1000000000
┌────────────────────────────────────────────────────────────────────────────────────┐
│                                     QUERY PLAN                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│ Sort (actual time=31387.960..43111.072 rows=10000000 loops=1)                      │
│   Output: c1, c2                                                                   │
│   Sort Key: toto.c2                                                                │
│   Sort Method: quicksort  Memory: 1171342kB                                        │
│   ->  Seq Scan on public.toto (actual time=0.025..12888.501 rows=10000000 loops=1) │
│         Output: c1, c2                                                             │
│ Settings: random_page_cost = '1.1', work_mem = '1000000000kB'                      │
│ Query Identifier: 2507635424379213761                                              │
│ Planning Time: 0.392 ms                                                            │
│ Execution Time: 54615.882 ms                                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
(10 rows)

Time: 54616.922 ms (00:54.617)
```

Cette fois, la configuration spécifique est bien appliquée. Pour que cela soit
appliqué en permanence, il convient de charger la bibliothèque dès le démarrage
de PostgreSQL grâce au paramètre `shared_preload_libraries`.

