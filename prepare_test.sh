create table test_pgqs as select n from generate_series(1,10) n;
prepare reqp(int) as select * from test_pgqs where n<$1;

explain (verbose, settings) execute reqp(1);
insert into pgqs_config values (8838756520854242449,'work_mem','1MB');

explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
explain (verbose, settings) execute reqp(1);
