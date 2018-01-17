# Copyright 2018 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import collections

from . import base
from .result import RerunResult


class RerunProc(base.TestProcProducer):
  def __init__(self, rerun_max, rerun_max_total=None):
    super(RerunProc, self).__init__('Rerun')
    self._rerun = {}
    self._results = collections.defaultdict(list)
    self._rerun_max = rerun_max
    self._rerun_total_left = rerun_max_total

  def _next_test(self, test):
    self._send_next_subtest(test)

  def _result_for(self, test, subtest, result):
    # First result
    if subtest.procid[-2:] == '-1':
      # Passed, no reruns
      if not result.has_unexpected_output:
        self._send_result(test, result)
        return

      self._rerun[test.procid] = 0

    results = self._results[test.procid]
    results.append(result)

    if self._needs_rerun(test, result):
      self._rerun[test.procid] += 1
      if self._rerun_total_left is not None:
        self._rerun_total_left -= 1
      self._send_next_subtest(test, self._rerun[test.procid])
    else:
      result = RerunResult.create(results)
      self._finalize_test(test)
      self._send_result(test, result)

  def _needs_rerun(self, test, result):
    # TODO(majeski): Limit reruns count for slow tests.
    return ((self._rerun_total_left is None or self._rerun_total_left > 0) and
            self._rerun[test.procid] < self._rerun_max and
            result.has_unexpected_output)

  def _send_next_subtest(self, test, run=0):
    subtest = self._create_subtest(test, str(run + 1))
    self._send_test(subtest)

  def _finalize_test(self, test):
    del self._rerun[test.procid]
    del self._results[test.procid]