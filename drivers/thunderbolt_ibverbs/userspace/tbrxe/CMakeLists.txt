# tbrxe is the stock rxe provider compiled under a different identity (see
# tbrxe.c, which #includes ../rxe/rxe.c). Mirror the rxe provider's LTTng
# arrangement so the wholesale include also builds where tracing is on.
if (ENABLE_LTTNG AND LTTNGUST_FOUND)
  set(TRACE_FILE ../rxe/rxe_trace.c)
endif()

rdma_provider(tbrxe
  ${TRACE_FILE}
  tbrxe.c
  )

if (ENABLE_LTTNG AND LTTNGUST_FOUND)
	target_include_directories("tbrxe-rdmav${IBVERBS_PABI_VERSION}" PUBLIC "../rxe")
	target_link_libraries("tbrxe-rdmav${IBVERBS_PABI_VERSION}" LINK_PRIVATE LTTng::UST)
endif()
