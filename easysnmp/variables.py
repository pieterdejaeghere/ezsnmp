from __future__ import unicode_literals

from .compat import urepr
from .helpers import normalize_oid
from .utils import strip_non_printable, tostr


class SNMPVariable(object):
    """
    An SNMP variable binding which is used to represent a piece of
    information being retreived via SNMP.

    :param oid: the OID being manipulated
    :param oid_index: the index of the OID
    :param value: the OID value
    :param snmp_type: the snmp_type of data contained in val (please see
                      http://www.net-snmp.org/wiki/index.php/TUT:snmpset#Data_Types
                      for further information); in the case that an object
                      or instance is not found, the type will be set to
                      NOSUCHOBJECT and NOSUCHINSTANCE respectively
    """
    TYPE_CONVERSIONS = {
        'INTEGER': int,
        'INTEGER32': int,
        'UNSIGNED32': int,
        'COUNTER': int,
        'GAUGE': int,
        'COUNTER64': int,
        'UINTEGER': int,
        'TICKS':int
    }

    def __init__(self, oid=None, oid_index=None, value=None, snmp_type=None):
        self.oid, self.oid_index = normalize_oid(oid, oid_index)
        self.value = value
        self.snmp_type = snmp_type
        self.root_oid = None

    def __repr__(self):
        printable_value = strip_non_printable(self.value)
        return (
            "<{0} value={1} (oid={2}, oid_index={3}, snmp_type={4}, root_oid={5})>".format(
                self.__class__.__name__,
                urepr(printable_value), urepr(self.oid),
                urepr(self.oid_index), urepr(self.snmp_type),
                urepr(self.root_oid)
            )
        )

    def __setattr__(self, name, value):
        self.__dict__[name] = tostr(value)

    def real_value(self):
        """
        Returns type converted value
        """
        return SNMPVariable.type_convert(self.snmp_type, self.value)

    @classmethod
    def type_convert(cls, type, value):
        if type in SNMPVariable.TYPE_CONVERSIONS:
            return SNMPVariable.TYPE_CONVERSIONS[type](value)
        else:
            return value


class SNMPVariableList(list):
    """
    An slight variation of a list which is used internally by the
    Net-SNMP C interface.
    """

    @property
    def varbinds(self):
        return self
