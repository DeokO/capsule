import sys, os
import numpy as np
from collections import defaultdict

dat_dir = sys.argv[1]
fit_dir = sys.argv[2]

if sys.argv[3] == 'final':
    iter_id = 'final'
else:
    iter_id = '%04d' % int(sys.argv[3])

epsilon = np.loadtxt(os.path.join(fit_dir, 'epsilon-%s.dat' % iter_id))
theta = np.loadtxt(os.path.join(fit_dir, 'theta-%s.dat' % iter_id))[:,1:]
zeta = np.loadtxt(os.path.join(fit_dir, 'zeta-%s.dat' % iter_id))[:,1:]

Nwords = defaultdict(int)
for line in open(os.path.join(dat_dir, "train.tsv")):
    doc, term, count = [int(t) for t in line.strip().split('\t')]
    Nwords[doc] += count

#cumulative = np.zeros(max(Nwords.keys())+1) # event only
cumulative = np.sum(theta,1) + np.sum(zeta,1)
for doc, time, eps, feps in epsilon:
    cumulative[int(doc)] += feps


eventness = defaultdict(float)
dc = defaultdict(float)
eventness2 = defaultdict(float)
dc2 = defaultdict(float)
eventness3 = defaultdict(float)
dc3 = defaultdict(float)
for doc, time, eps, feps in epsilon:
    eventness[int(time)] += feps / cumulative[int(doc)]
    dc[int(time)] += feps / eps
    eventness2[int(time)] += (feps / cumulative[int(doc)]) * Nwords[int(doc)]
    dc2[int(time)] += (feps / eps) * Nwords[int(doc)]
    eventness3[int(time)] += feps
    dc3[int(time)] += 1.0

for time in eventness:
    #print time, eventness[time], dc[time], (eventness[time]/dc[time]), (
    print time, (eventness[time]/dc[time]), (eventness2[time]/dc2[time]), (eventness3[time]/dc3[time])



'''
i0  284 5.076742e-01    4.167243e-02
0   285 3.780185e-01    1.083041e-01
0   286 1.998536e-01    1.998536e-01
1   23  8.387790e-01    1.972619e-02
1   24  7.229748e-01    5.934538e-02
1   25  4.927697e-01    1.411809e-0

for line in open(os.path.join(fit_dir, "eta-%s.dat" % iter_id)):
docmap = {}
for line in open("/home/statler/achaney/dat/DE/cable_data/pro/may23/weekly_bodyandsubj1976/doc_ids.tsv"):
    doc, idx = line.strip().split('\t')
    docmap[int(doc)] = idx
entity = {}
date = {}
for line in open("/home/statler/achaney/dat/DE/cable_data/pro/may23/weekly_bodyandsubj1976/meta.tsv"):
    doc, author, time = line.strip().split('\t')
    entity[int(doc)] = int(author)
    date[int(doc)] = int(time)


scores = defaultdict(lambda: defaultdict(float))
counts = defaultdict(lambda: defaultdict(int))
for line in open("/home/statler/achaney/dat/DE/cable_data/pro/may23/weekly_bodyandsubj1976/train.tsv"):
    doc, term, count = [int(v) for v in line.strip().split('\t')]
    scores[date[doc]][doc] += pi[date[doc], term] * count
    counts[date[doc]][doc] += count



for week in range(53):
    sc = 0.0
    c = 0
    for s in scores[week]:
        sc += scores[week][s] / counts[week][s]
        c +=1
    print week, sc, c, (sc/c)
    #scores[week], counts[week], (scores[week]/counts[week])'''