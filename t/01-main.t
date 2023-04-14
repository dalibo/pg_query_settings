#!/usr/bin/perl

use strict;
use warnings;
use lib 't/lib';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('prod');
print "Init node\n";
$node->init();
print "Node created\n";

my $out;

$node->append_conf('postgresql.conf', "compute_query_id = on")
    if $node->pg_version >= 14;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_stat_statements'");

$node->start;

### Beginning of tests ###

$node->safe_psql('postgres', "CREATE EXTENSION pg_query_settings");
$node->safe_psql('postgres', "CREATE EXTENSION pg_stat_statements");
$node->safe_psql('postgres', "LOAD 'pg_query_settings'");
$node->safe_psql('postgres', "SELECT relname FROM pg_class LIMIT 1");

($out) = $node->safe_psql('postgres', "SELECT queryid FROM pg_stat_statements WHERE query = 'SELECT relname FROM pg_class LIMIT \$1';");

print "queryid is $out\n";

done_testing();

### End of tests ###

$node->stop('fast');
