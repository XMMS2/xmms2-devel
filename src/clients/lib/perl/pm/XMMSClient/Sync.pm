package Audio::XMMSClient::Sync;

use strict;
use warnings;
use Audio::XMMSClient;
use Scalar::Util qw/blessed/;

sub new {
    my $base = shift;

    my $c = Audio::XMMSClient->new( @_ );

    my $self = bless \$c, $base;

    return $self;
}

our $AUTOLOAD;
sub AUTOLOAD {
    my ($self) = @_;

    (my $func = $AUTOLOAD) =~ s/.*:://;

    unless ($$self->can($func)) {
        croak (qq{Can't locate object method "$func" via package Audio::XMMSClient::Sync});
    }

    {
        no strict 'refs';
        *$AUTOLOAD = sub { shift->sync_request( $func, @_ ) };

        &$AUTOLOAD;
    }
}

sub sync_request {
    my ($self, $request, @args) = @_;

    my $resp = $$self->$request(@args);

    if (blessed $resp && $resp->isa('Audio::XMMSClient::Result')) {
        $resp->wait;

        if ($resp->iserror) {
            croak ($resp->get_error);
        }

        $resp = $resp->value;
    }

    return $resp;
}

1;
