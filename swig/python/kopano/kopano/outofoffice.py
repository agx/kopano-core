"""
Part of the high-level python bindings for Kopano

Copyright 2005 - 2016 Zarafa and its licensors (see LICENSE file for details)
Copyright 2016 - Kopano and its licensors (see LICENSE file for details)
"""

import time
import datetime

from MAPI import KEEP_OPEN_READWRITE
from MAPI.Tags import (
    PR_EC_OUTOFOFFICE, PR_EC_OUTOFOFFICE_SUBJECT_W,
    PR_EC_OUTOFOFFICE_MSG_W, PR_EC_OUTOFOFFICE_FROM,
    PR_EC_OUTOFOFFICE_UNTIL
)
from MAPI.Struct import SPropValue, MAPIErrorNotFound
from MAPI.Time import unixtime

from .compat import repr as _repr, fake_unicode as _unicode

class OutOfOffice(object):
    """OutOfOffice class

    Class which contains a :class:`store <Store>` out of office properties and
    can set out-of-office status, message and subject.

    :param store: :class:`store <Store>`
    """
    def __init__(self, store):
        self.store = store

    @property
    def enabled(self):
        """ Out of office enabled status """

        try:
            return self.store.prop(PR_EC_OUTOFOFFICE).value
        except MAPIErrorNotFound:
            return False

    @enabled.setter
    def enabled(self, value):
        self.store.mapiobj.SetProps([SPropValue(PR_EC_OUTOFOFFICE, value)])
        self.store.mapiobj.SaveChanges(KEEP_OPEN_READWRITE)

    @property
    def subject(self):
        """ Subject """

        try:
            return self.store.prop(PR_EC_OUTOFOFFICE_SUBJECT_W).value
        except MAPIErrorNotFound:
            return u''

    @subject.setter
    def subject(self, value):
        self.store.mapiobj.SetProps([SPropValue(PR_EC_OUTOFOFFICE_SUBJECT_W, _unicode(value))])
        self.store.mapiobj.SaveChanges(KEEP_OPEN_READWRITE)

    @property
    def message(self):
        """ Message """

        try:
            return self.store.prop(PR_EC_OUTOFOFFICE_MSG_W).value
        except MAPIErrorNotFound:
            return u''

    @message.setter
    def message(self, value):
        self.store.mapiobj.SetProps([SPropValue(PR_EC_OUTOFOFFICE_MSG_W, _unicode(value))])
        self.store.mapiobj.SaveChanges(KEEP_OPEN_READWRITE)

    @property
    def start(self):
        """ Out-of-office is activated from the particular datetime onwards """
        try:
            return self.store.prop(PR_EC_OUTOFOFFICE_FROM).value
        except MAPIErrorNotFound:
            pass

    @start.setter
    def start(self, value):
        if value is None:
            self.store.mapiobj.DeleteProps([PR_EC_OUTOFOFFICE_FROM])
        else:
            value = unixtime(time.mktime(value.timetuple()))
            self.store.mapiobj.SetProps([SPropValue(PR_EC_OUTOFOFFICE_FROM, value)])
        self.store.mapiobj.SaveChanges(KEEP_OPEN_READWRITE)

    @property
    def end(self):
        """ Out-of-office is activated until the particular datetime """
        try:
            return self.store.prop(PR_EC_OUTOFOFFICE_UNTIL).value
        except MAPIErrorNotFound:
            pass

    @end.setter
    def end(self, value):
        if value is None:
            self.store.mapiobj.DeleteProps([PR_EC_OUTOFOFFICE_UNTIL])
        else:
            value = unixtime(time.mktime(value.timetuple()))
            self.store.mapiobj.SetProps([SPropValue(PR_EC_OUTOFOFFICE_UNTIL, value)])
        self.store.mapiobj.SaveChanges(KEEP_OPEN_READWRITE)

    @property
    def period_desc(self): # XXX class Period?
        terms = []
        if self.start:
            terms.append('from %s' % self.start)
        if self.end:
            terms.append('until %s' % self.end)
        return ' '.join(terms)

    @property
    def active(self):
        if not self.enabled:
            return False
        now = datetime.datetime.now()
        if self.start and now < self.start:
            return False
        if self.end and now >= self.end:
            return False
        return True

    def __unicode__(self):
        return u'OutOfOffice(%s)' % self.subject

    def __repr__(self):
        return _repr(self)

    def update(self, **kwargs):
        """ Update function for outofoffice """

        for key, val in kwargs.items():
            setattr(self, key, val)

