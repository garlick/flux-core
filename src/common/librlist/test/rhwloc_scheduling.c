/************************************************************\
 * Copyright 2026 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* Test rhwloc_scheduling() and TreePool topology generation */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <string.h>
#include <jansson.h>

#include "src/common/libtap/tap.h"
#include "src/common/libutil/errprintf.h"
#include "rhwloc.h"

/* NPS1 single package: 2 cores, 8 GiB */
static const char xml_nps1_1pkg[] = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE topology SYSTEM \"hwloc.dtd\">\n\
<topology>\n\
  <object type=\"Machine\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
    <info name=\"HostName\" value=\"testhost\"/>\n\
    <object type=\"NUMANode\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\" local_memory=\"8589934592\">\n\
      <object type=\"Package\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
        <object type=\"Core\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
        <object type=\"Core\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
      </object>\n\
    </object>\n\
  </object>\n\
</topology>\n";

/* NPS1 two packages: 2 sockets, each with 2 cores and 8 GiB */
static const char xml_nps1_2pkg[] = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE topology SYSTEM \"hwloc.dtd\">\n\
<topology>\n\
  <object type=\"Machine\" os_index=\"0\"\
 cpuset=\"0x0000000f\" complete_cpuset=\"0x0000000f\"\
 online_cpuset=\"0x0000000f\" allowed_cpuset=\"0x0000000f\"\
 nodeset=\"0x00000003\" complete_nodeset=\"0x00000003\"\
 allowed_nodeset=\"0x00000003\">\n\
    <info name=\"HostName\" value=\"testhost\"/>\n\
    <object type=\"NUMANode\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\" local_memory=\"8589934592\">\n\
      <object type=\"Package\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
        <object type=\"Core\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
        <object type=\"Core\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
      </object>\n\
    </object>\n\
    <object type=\"NUMANode\" os_index=\"1\"\
 cpuset=\"0x0000000c\" complete_cpuset=\"0x0000000c\"\
 online_cpuset=\"0x0000000c\" allowed_cpuset=\"0x0000000c\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\" local_memory=\"8589934592\">\n\
      <object type=\"Package\" os_index=\"1\"\
 cpuset=\"0x0000000c\" complete_cpuset=\"0x0000000c\"\
 online_cpuset=\"0x0000000c\" allowed_cpuset=\"0x0000000c\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\">\n\
        <object type=\"Core\" os_index=\"2\"\
 cpuset=\"0x00000004\" complete_cpuset=\"0x00000004\"\
 online_cpuset=\"0x00000004\" allowed_cpuset=\"0x00000004\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\">\n\
          <object type=\"PU\" os_index=\"2\"\
 cpuset=\"0x00000004\" complete_cpuset=\"0x00000004\"\
 online_cpuset=\"0x00000004\" allowed_cpuset=\"0x00000004\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\"/>\n\
        </object>\n\
        <object type=\"Core\" os_index=\"3\"\
 cpuset=\"0x00000008\" complete_cpuset=\"0x00000008\"\
 online_cpuset=\"0x00000008\" allowed_cpuset=\"0x00000008\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\">\n\
          <object type=\"PU\" os_index=\"3\"\
 cpuset=\"0x00000008\" complete_cpuset=\"0x00000008\"\
 online_cpuset=\"0x00000008\" allowed_cpuset=\"0x00000008\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\"/>\n\
        </object>\n\
      </object>\n\
    </object>\n\
  </object>\n\
</topology>\n";

/* No packages: 2 NUMA nodes with cores directly beneath, 4 GiB each */
static const char xml_no_pkg[] = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE topology SYSTEM \"hwloc.dtd\">\n\
<topology>\n\
  <object type=\"Machine\" os_index=\"0\"\
 cpuset=\"0x0000000f\" complete_cpuset=\"0x0000000f\"\
 online_cpuset=\"0x0000000f\" allowed_cpuset=\"0x0000000f\"\
 nodeset=\"0x00000003\" complete_nodeset=\"0x00000003\"\
 allowed_nodeset=\"0x00000003\">\n\
    <info name=\"HostName\" value=\"testhost\"/>\n\
    <object type=\"NUMANode\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\" local_memory=\"4294967296\">\n\
      <object type=\"Core\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
        <object type=\"PU\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
      </object>\n\
      <object type=\"Core\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
        <object type=\"PU\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
      </object>\n\
    </object>\n\
    <object type=\"NUMANode\" os_index=\"1\"\
 cpuset=\"0x0000000c\" complete_cpuset=\"0x0000000c\"\
 online_cpuset=\"0x0000000c\" allowed_cpuset=\"0x0000000c\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\" local_memory=\"4294967296\">\n\
      <object type=\"Core\" os_index=\"2\"\
 cpuset=\"0x00000004\" complete_cpuset=\"0x00000004\"\
 online_cpuset=\"0x00000004\" allowed_cpuset=\"0x00000004\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\">\n\
        <object type=\"PU\" os_index=\"2\"\
 cpuset=\"0x00000004\" complete_cpuset=\"0x00000004\"\
 online_cpuset=\"0x00000004\" allowed_cpuset=\"0x00000004\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\"/>\n\
      </object>\n\
      <object type=\"Core\" os_index=\"3\"\
 cpuset=\"0x0000000c\" complete_cpuset=\"0x0000000c\"\
 online_cpuset=\"0x0000000c\" allowed_cpuset=\"0x0000000c\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\">\n\
        <object type=\"PU\" os_index=\"3\"\
 cpuset=\"0x00000008\" complete_cpuset=\"0x00000008\"\
 online_cpuset=\"0x00000008\" allowed_cpuset=\"0x00000008\"\
 nodeset=\"0x00000002\" complete_nodeset=\"0x00000002\"\
 allowed_nodeset=\"0x00000002\"/>\n\
      </object>\n\
    </object>\n\
  </object>\n\
</topology>\n";

/* GPU under NUMANode: single package with 1 GPU */
static const char xml_nps1_gpu[] = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE topology SYSTEM \"hwloc.dtd\">\n\
<topology>\n\
  <object type=\"Machine\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
    <info name=\"HostName\" value=\"testhost\"/>\n\
    <object type=\"NUMANode\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\" local_memory=\"8589934592\">\n\
      <object type=\"Package\" os_index=\"0\"\
 cpuset=\"0x00000003\" complete_cpuset=\"0x00000003\"\
 online_cpuset=\"0x00000003\" allowed_cpuset=\"0x00000003\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
        <object type=\"Core\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"0\"\
 cpuset=\"0x00000001\" complete_cpuset=\"0x00000001\"\
 online_cpuset=\"0x00000001\" allowed_cpuset=\"0x00000001\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
        <object type=\"Core\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\">\n\
          <object type=\"PU\" os_index=\"1\"\
 cpuset=\"0x00000002\" complete_cpuset=\"0x00000002\"\
 online_cpuset=\"0x00000002\" allowed_cpuset=\"0x00000002\"\
 nodeset=\"0x00000001\" complete_nodeset=\"0x00000001\"\
 allowed_nodeset=\"0x00000001\"/>\n\
        </object>\n\
      </object>\n\
    </object>\n\
    <object type=\"Bridge\" os_index=\"0\" bridge_type=\"0-1\" depth=\"0\"\
 bridge_pci=\"0000:[00-01]\">\n\
      <object type=\"PCIDev\" os_index=\"4096\" name=\"Test GPU 0\"\
 pci_busid=\"0000:01:00.0\" pci_type=\"0302 [10de:1234] [10de:0000] a1\"\
 pci_link_speed=\"0.000000\">\n\
        <object type=\"OSDev\" name=\"cuda0\" osdev_type=\"5\">\n\
          <info name=\"CoProcType\" value=\"CUDA\"/>\n\
          <info name=\"Backend\" value=\"CUDA\"/>\n\
        </object>\n\
      </object>\n\
    </object>\n\
  </object>\n\
</topology>\n";

/* Helper: extract topo object from scheduling result */
static json_t *scheduling_topo (json_t *sched)
{
    json_t *children = json_object_get (sched, "children");
    json_t *entry = json_array_get (children, 0);
    return json_object_get (entry, "topo");
}

/* Test basic TreePool topology generation */
void test_basic_topologies (void)
{
    hwloc_topology_t topo;
    json_t *result, *t, *arr, *e;
    const char *s;

    /* NPS1 single package: single-element socket collapses to flat leaf */
    topo = rhwloc_xml_topology_load (xml_nps1_1pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_1pkg topology");
    result = rhwloc_scheduling (topo, "TreePool", "0", NULL);
    hwloc_topology_destroy (topo);
    ok (result != NULL,
        "NPS1 1-pkg returns non-NULL");
    ok (strcmp (json_string_value (json_object_get (result, "writer")),
                "TreePool") == 0,
        "NPS1 1-pkg writer == \"TreePool\"");
    t = scheduling_topo (result);
    ok (t != NULL, "NPS1 1-pkg topo present");
    ok (json_object_get (t, "socket") == NULL,
        "NPS1 1-pkg topo has no socket array (collapsed)");
    s = json_string_value (json_object_get (t, "cores"));
    ok (s && strcmp (s, "0-1") == 0,
        "NPS1 1-pkg topo.cores == \"0-1\"");
    ok (json_integer_value (json_object_get (t, "memory")) == 8,
        "NPS1 1-pkg topo.memory == 8");
    json_decref (result);

    /* NPS1 two packages: two socket entries, each folding its sole NUMA */
    topo = rhwloc_xml_topology_load (xml_nps1_2pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_2pkg topology");
    result = rhwloc_scheduling (topo, "TreePool", "0", NULL);
    hwloc_topology_destroy (topo);
    ok (result != NULL,
        "NPS1 2-pkg returns non-NULL");
    t = scheduling_topo (result);
    arr = json_object_get (t, "socket");
    ok (json_is_array (arr) && json_array_size (arr) == 2,
        "NPS1 2-pkg topo.socket has 2 entries");
    e = json_array_get (arr, 0);
    s = json_string_value (json_object_get (e, "cores"));
    ok (s && strcmp (s, "0-1") == 0,
        "NPS1 2-pkg topo.socket[0].cores == \"0-1\"");
    ok (json_integer_value (json_object_get (e, "memory")) == 8,
        "NPS1 2-pkg topo.socket[0].memory == 8");
    e = json_array_get (arr, 1);
    s = json_string_value (json_object_get (e, "cores"));
    ok (s && strcmp (s, "2-3") == 0,
        "NPS1 2-pkg topo.socket[1].cores == \"2-3\"");
    ok (json_integer_value (json_object_get (e, "memory")) == 8,
        "NPS1 2-pkg topo.socket[1].memory == 8");
    json_decref (result);

    /* No packages: two NUMA nodes directly in topo */
    topo = rhwloc_xml_topology_load (xml_no_pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_no_pkg topology");
    result = rhwloc_scheduling (topo, "TreePool", "0", NULL);
    hwloc_topology_destroy (topo);
    ok (result != NULL,
        "no-pkg returns non-NULL");
    t = scheduling_topo (result);
    arr = json_object_get (t, "numa");
    ok (json_is_array (arr) && json_array_size (arr) == 2,
        "no-pkg topo.numa has 2 entries");
    e = json_array_get (arr, 0);
    s = json_string_value (json_object_get (e, "cores"));
    ok (s && strcmp (s, "0-1") == 0,
        "no-pkg topo.numa[0].cores == \"0-1\"");
    ok (json_integer_value (json_object_get (e, "memory")) == 4,
        "no-pkg topo.numa[0].memory == 4");
    e = json_array_get (arr, 1);
    s = json_string_value (json_object_get (e, "cores"));
    ok (s && strcmp (s, "2-3") == 0,
        "no-pkg topo.numa[1].cores == \"2-3\"");
    ok (json_integer_value (json_object_get (e, "memory")) == 4,
        "no-pkg topo.numa[1].memory == 4");
    json_decref (result);

    /* GPU under NUMANode: single socket collapses, gpus at topo level */
    topo = rhwloc_xml_topology_load (xml_nps1_gpu, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_gpu topology");
    /* Check how many GPUs hwloc finds in the topology */
    {
        int ngpus = 0;
        hwloc_obj_t *gpus = rhwloc_gpu_objects (topo, &ngpus);
        diag ("rhwloc_gpu_objects found %d GPU(s) in xml_nps1_gpu", ngpus);
        free (gpus);
    }
    result = rhwloc_scheduling (topo, "TreePool", "0", NULL);
    hwloc_topology_destroy (topo);
    ok (result != NULL,
        "GPU topology returns non-NULL");
    if (result) {
        char *debug_result = json_dumps (result, JSON_INDENT(2));
        diag ("full result JSON:\n%s", debug_result);
        free (debug_result);
    }
    t = scheduling_topo (result);
    if (t) {
        char *debug_topo = json_dumps (t, JSON_INDENT(2));
        diag ("topo object:\n%s", debug_topo);
        free (debug_topo);
    }
    s = json_string_value (json_object_get (t, "gpus"));
    diag ("topo.gpus = %s", s ? s : "NULL");
    ok (s && strcmp (s, "0") == 0,
        "GPU topology topo.gpus == \"0\"");
    ok (json_integer_value (json_object_get (t, "memory")) == 8,
        "GPU topology topo.memory == 8");
    json_decref (result);
}

/* Test error paths and errno handling */
void test_error_paths (void)
{
    hwloc_topology_t topo;
    json_t *result;
    flux_error_t err;

    /* Unknown format returns NULL with EINVAL */
    topo = rhwloc_xml_topology_load (xml_nps1_1pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_1pkg topology");
    errno = 0;
    result = rhwloc_scheduling (topo, "Unknown", "0", &err);
    ok (result == NULL,
        "unknown format returns NULL");
    ok (errno == EINVAL,
        "unknown format sets errno=EINVAL (got %d)", errno);
    ok (strlen (err.text) > 0,
        "unknown format sets error message");
    diag ("error message: %s", err.text);
    hwloc_topology_destroy (topo);

    /* NULL topology */
    errno = 0;
    memset (&err, 0, sizeof (err));
    result = rhwloc_scheduling (NULL, "TreePool", "0", &err);
    ok (result == NULL,
        "NULL topology returns NULL");
    ok (errno == EINVAL,
        "NULL topology sets errno=EINVAL (got %d)", errno);

    /* NULL ranks */
    topo = rhwloc_xml_topology_load (xml_nps1_1pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_1pkg topology");
    errno = 0;
    memset (&err, 0, sizeof (err));
    result = rhwloc_scheduling (topo, "TreePool", NULL, &err);
    ok (result == NULL,
        "NULL ranks returns NULL");
    ok (errno == EINVAL,
        "NULL ranks sets errno=EINVAL (got %d)", errno);
    hwloc_topology_destroy (topo);
}

/* Test children array structure and rank assignment */
void test_children_structure (void)
{
    hwloc_topology_t topo;
    json_t *result, *children, *entry, *t;
    const char *ranks_str;

    topo = rhwloc_xml_topology_load (xml_nps1_2pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_2pkg topology");
    result = rhwloc_scheduling (topo, "TreePool", "0-3", NULL);
    hwloc_topology_destroy (topo);
    ok (result != NULL,
        "scheduling with rank range returns non-NULL");

    children = json_object_get (result, "children");
    ok (json_is_array (children),
        "result has children array");
    ok (json_array_size (children) == 1,
        "children array has 1 entry");

    entry = json_array_get (children, 0);
    ok (entry != NULL,
        "children[0] exists");

    ranks_str = json_string_value (json_object_get (entry, "ranks"));
    ok (ranks_str && strcmp (ranks_str, "0-3") == 0,
        "children[0].ranks == \"0-3\"");

    t = json_object_get (entry, "topo");
    ok (t != NULL,
        "children[0].topo exists");

    json_decref (result);
}

/* Test writer field */
void test_writer_field (void)
{
    hwloc_topology_t topo;
    json_t *result;
    const char *writer;

    topo = rhwloc_xml_topology_load (xml_nps1_1pkg, RHWLOC_NO_RESTRICT);
    if (!topo)
        BAIL_OUT ("failed to load xml_nps1_1pkg topology");
    result = rhwloc_scheduling (topo, "TreePool", "0", NULL);
    hwloc_topology_destroy (topo);

    ok (result != NULL,
        "scheduling returns non-NULL");

    writer = json_string_value (json_object_get (result, "writer"));
    ok (writer != NULL,
        "result has writer field");
    ok (strcmp (writer, "TreePool") == 0,
        "writer == \"TreePool\"");

    json_decref (result);
}

int main (int ac, char *av[])
{
    plan (NO_PLAN);

    test_basic_topologies ();
    test_error_paths ();
    test_children_structure ();
    test_writer_field ();

    done_testing ();
}

/* vi: ts=4 sw=4 expandtab
 */
