package OdbParser;

use 5.008008;
use strict;
use warnings;

require Exporter;

our @ISA = qw(Exporter);

our $VERSION = '0.01';

use Carp;

use constant ODB_OPCODE_TORRENT         => 254;
use constant ODB_OPCODE_EOF             => 255;

my %def_callbacks = (
    "start_odb"         => \&def_start_odb,
    "parse_torrent"     => \&def_parse_torrent,
    "parse_peer"        => \&def_parse_peer,
    "end_odb"           => \&def_end_odb,
);

sub new {
    my ($class, $callbacks) = @_;
    $callbacks ||= \%def_callbacks;

    my $self = bless {}, $class;
    $self->{callback} = $callbacks;
    $self;
}

sub verify_magic {
    my ($self, $magic) = @_;
    if ($magic ne 'OPENTRACKER') {
        croak "Invalid File Format for file " . $self->{filename};
    }
}

sub verify_version {
    my ($self, $version) = @_;

    $version = int($version);

    if ($version != 1) {
        croak "Invalid ODB version $version for file " . $self->{filename};
    }
}

sub invoke_callback {
    my $self = shift;
    my $method = shift;
    my @args = @_;

    if (defined($self->{callback}->{$method})) {
        my $func = $self->{callback}->{$method};
        &$func(@args);
    }
}

sub parse {
    my $self = shift;
    my $filename = shift;

    unless (defined($filename)) {
        croak "Expected a Opentracker dump file name";
    }

    $self->{filename} = $filename;

    my $buffer;
    open my $INFH, $filename or 
        croak "Open $filename for reading failed: $!";
    binmode $INFH;

    read($INFH, $buffer, 11) or croak "Read $filename failed: $!";
    $self->verify_magic($buffer);

    read($INFH, $buffer, 4) or croak "Read $filename failed: $!";
    $self->verify_version($buffer);

    $self->invoke_callback("start_odb", $filename);

    my $is_first_torrent = 1;

    while (1)  {
        my $flag = &read_unsigned_char($INFH);

        if ($flag == ODB_OPCODE_TORRENT) {
            my $info_hash;
            read($INFH, $info_hash, 20) or croak "READ $filename failed: $!";
            my $last_accessed = &read_unsigned_long($INFH);
            my $seed_count = &read_unsigned_long($INFH);
            my $total_count = &read_unsigned_long($INFH);
            my $download_time = &read_unsigned_long($INFH);
            my $peer_count = &read_unsigned_int($INFH);
            my $info_hash_str = unpack("h*20", $info_hash);

            $self->invoke_callback("parse_torrent", $info_hash, $info_hash_str,
                $last_accessed, $seed_count, $total_count, $download_time,
                $peer_count);

            for (my $i = 0; $i < $peer_count; $i++) {
                my $ip = &read_big_endian_unsigned_int($INFH);
                my $ip_str = sprintf("%d.%d.%d.%d", $ip >> 24, 
                    ($ip >> 16) & 0x00ff, ($ip >> 8) & 0x0000ff,
                    ($ip) & 0x000000ff);
                my $port = &read_big_endian_unsigned_short($INFH);
                my $dummy;
                read($INFH, $dummy, 2) or croak "READ $filename failed: $!";

                $self->invoke_callback("parse_peer", $ip_str, $port);
            }
        } elsif ($flag == ODB_OPCODE_EOF) {
            $self->invoke_callback("end_odb", $filename);
            last;
        } else {
            croak "Invalid ODB format";
        }
    }
}

sub read_unsigned_char {
    my ($fh) = @_;
    my $buffer;
    read($fh, $buffer, 1) or croak "read failed: $!";
    return unpack('C', $buffer);
}

sub read_unsigned_long {
    my ($fh) = @_;
    my $buffer;
    read($fh, $buffer, 8) or croak "read failed: $!";
    return unpack('Q', $buffer);
}

sub read_unsigned_int {
    my ($fh) = @_;
    my $buffer;
    read($fh, $buffer, 4) or croak "read failed: $!";
    return unpack('I', $buffer);
}

sub read_big_endian_unsigned_int {
    my ($fh) = @_;
    my $buffer;
    read($fh, $buffer, 4) or croak "read failed: $!";
    return unpack('N', $buffer);
}

sub read_big_endian_unsigned_short {
    my ($fh) = @_;
    my $buffer;
    read($fh, $buffer, 2) or croak "read failed: $!";
    return unpack('n', $buffer);
}

#===========================================================
# default callbacks
#===========================================================

sub def_start_odb {
    my $filename = shift;
    print "start parse ODB file: $filename\n";
}

sub def_parse_torrent {
    my $info_hash = shift;
    my $info_hash_str = shift;
    my $last_accessed = shift;
    my $seed_count = shift;
    my $total_count = shift;
    my $download_time = shift;
    my $peer_count = shift;

    printf("%s has %d peers\n", $info_hash_str, $peer_count);
}


sub def_parse_peer {
    my $ip_str = shift;
    my $port = shift;

    printf("peer: %s:%d\n", $ip_str, $port);
}

sub def_end_odb {
    my $filename = shift;
    print "end parse ODB file: $filename\n";
}


1;

__END__

=head1 NAME

OdbParser - opentracker dump file parser

=head1 VERSION

version 0.01

=head1 SYNOPSIS

 use OdbParser;

 my $filename = "/path/to/foo.odb";

 my $parser =  new OdbParser;
 #
 # or
 #
 my $callbacks = {
     "start_odb"     => \&start_odb,
     "parse_torrent" => \&parse_torrent,
     "parse_peer"    => \&parse_peer,
     "end_od"        => \&end_odb,
 };

 my $parser = new OdbParser($callbacks);

 $parser->parse($filename);


=head1 DESCRIPTION

OdbParser is a parser for Opentracker's odb dump files.


=head2 callbacks

The dump file is parsed sequentially. As and when torrent or peer is 
discovered, appropriate callback would be invoked. You can set the
callback item `undef` if you don't care it.

=over 4

=item start_odb

    sub start_rdb {
        my $filename = shift;
        # fill your code
    }

Called once when we start parsing a valid opentracker dump file.

=item parse_torrent

    sub parse_torrent {
        my $info_hash = shift;
        my $info_hash_str = shift;
        my $last_accessed = shift;
        my $seed_count = shift;
        my $total_count = shift;
        my $download_time = shift;
        my $peer_count = shift;

        # fill your code 
    }

Invoked when we have parsed a torrent.

=item parse_peer

    sub parse_peer {
        my $ip_str = shift;
        my $port = shift;
    
        # fill your code
    }

Invoked when we have parsed a peer.

=item end_odb

    sub end_odb {
        my $filename = shift;

        #fill your code
    }

=back

=head1 REPOSITORY

https://github.com/flygoast/opentracker

=head1 AUTHOR

FengGu, E<lt>flygoast@gmail.comE<gt>

Copyright (C) 2013 by FengGu

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself, either Perl version 5.8.8 or,
at your option, any later version of Perl 5 you may have available.


=cut
