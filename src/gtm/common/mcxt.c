/*-------------------------------------------------------------------------
 *
 * mcxt.c
 *	  POSTGRES memory context management code.
 *
 * This module handles context management operations that are independent
 * of the particular kind of context being operated on.  It calls
 * context-type-specific operations via the function pointers in a
 * context's MemoryContextMethods struct.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mmgr/mcxt.c,v 1.65 2008/06/28 16:45:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */


#include "gtm/gtm_c.h"
#include "gtm/memutils.h"
#include "gtm/elog.h"
#include "gtm/assert.h"
#include "gtm/gtm.h"


/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

/*
 * Standard top-level contexts. For a description of the purpose of each
 * of these contexts, refer to src/backend/utils/mmgr/README
 */

static void Gtm_MemoryContextStatsInternal(MemoryContext context, int level);
static void MemoryContextDeleteInternal(MemoryContext context, bool parent_locked);
void *allocTopMemCxt(size_t s);

MemoryContext	TopMostMemoryContext;

/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * Gtm_MemoryContextInit
 *		Start up the memory-context subsystem.
 *
 * This must be called before creating contexts or allocating memory in
 * contexts.  TopMemoryContext and ErrorContext are initialized here;
 * other contexts must be created afterwards.
 *
 * In normal multi-backend operation, this is called once during
 * postmaster startup, and not at all by individual backend startup
 * (since the backends inherit an already-initialized context subsystem
 * by virtue of being forked off the postmaster).
 *
 * In a standalone backend this must be called during backend startup.
 */
void
Gtm_MemoryContextInit(void)
{
	void *thrinfo;

	AssertState(TopMemoryContext == NULL);

	/*
	 * Initialize TopMemoryContext as an AllocSetContext with slow growth rate
	 * --- we don't really expect much to be allocated in it.
	 *
	 * (There is special-case code in Gtm_MemoryContextCreate() for this call.)
	 *
	 * This context is shared between different threads and must be made
	 * thread-safe
	 */
	TopMemoryContext = AllocSetContextCreate((MemoryContext) NULL,
											 "TopMemoryContext",
											 0,
											 8 * 1024,
											 8 * 1024,
											 true);

	TopMostMemoryContext = TopMemoryContext;

	/*
	 * Not having any other place to point CurrentMemoryContext, make it point
	 * to TopMemoryContext.  Caller should change this soon!
	 */
	thrinfo = GetMyThreadInfo;
	CurrentMemoryContext = TopMemoryContext;

	/*
	 * Initialize ErrorContext as an AllocSetContext with slow growth rate ---
	 * we don't really expect much to be allocated in it. More to the point,
	 * require it to contain at least 8K at all times. This is the only case
	 * where retained memory in a context is *essential* --- we want to be
	 * sure ErrorContext still has some memory even if we've run out
	 * elsewhere!
	 *
	 * Similar to TopMostMemoryContext, this context may as well be shared
	 * between threads
	 */
	ErrorContext = AllocSetContextCreate(TopMemoryContext,
										 "ErrorContext",
										 8 * 1024,
										 8 * 1024,
										 8 * 1024,
										 true);
}

/*
 * Gtm_MemoryContextReset
 *		Release all space allocated within a context and its descendants,
 *		but don't delete the contexts themselves.
 *
 * The type-specific reset routine handles the context itself, but we
 * have to do the recursion for the children.
 */
void
Gtm_MemoryContextReset(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	if (MemoryContextIsShared(context))
		MemoryContextLock(context);

	/* save a function call in common case where there are no children */
	if (context->firstchild != NULL)
		Gtm_Gtm_MemoryContextResetChildren(context);

	if (MemoryContextIsShared(context))
		MemoryContextUnlock(context);

	(*context->methods->reset) (context);
}

/*
 * Gtm_Gtm_MemoryContextResetChildren
 *		Release all space allocated within a context's descendants,
 *		but don't delete the contexts themselves.  The named context
 *		itself is not touched.
 */
void
Gtm_Gtm_MemoryContextResetChildren(MemoryContext context)
{
	MemoryContext child;

	AssertArg(MemoryContextIsValid(context));

	/*
	 * For a shared context, lock the parent context before resetting the
	 * children contextes
	 */
	if (MemoryContextIsShared(context))
		MemoryContextLock(context);

	for (child = context->firstchild; child != NULL; child = child->nextchild)
		Gtm_MemoryContextReset(child);

	if (MemoryContextIsShared(context))
		MemoryContextUnlock(context);
}

/*
 * MemoryContextDelete
 *		Delete a context and its descendants, and release all space
 *		allocated therein.
 *
 * The type-specific delete routine removes all subsidiary storage
 * for the context, but we have to delete the context node itself,
 * as well as recurse to get the children.	We must also delink the
 * node from its parent, if it has one.
 */
static void
MemoryContextDeleteInternal(MemoryContext context, bool parent_locked)
{
	AssertArg(MemoryContextIsValid(context));
	/* We had better not be deleting TopMemoryContext ... */
	Assert(context != TopMostMemoryContext);
	/* And not CurrentMemoryContext, either */
	Assert(context != CurrentMemoryContext);

	Gtm_MemoryContextDeleteChildren(context);

	/*
	 * We delink the context from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted contexts still attached
	 * to the context tree.  Better a leak than a crash.
	 */
	if (context->parent)
	{
		MemoryContext parent = context->parent;

		/*
		 * If the parent context is shared and is already locked by the caller,
		 * no need to relock again. In fact, that's not the right thing to do
		 * since it will lead to a self-deadlock
		 */
		if (MemoryContextIsShared(parent) && (!parent_locked))
			MemoryContextLock(parent);

		if (context == parent->firstchild)
			parent->firstchild = context->nextchild;
		else
		{
			MemoryContext child;

			for (child = parent->firstchild; child; child = child->nextchild)
			{
				if (context == child->nextchild)
				{
					child->nextchild = context->nextchild;
					break;
				}
			}
		}

		if (MemoryContextIsShared(parent) && (!parent_locked))
			MemoryContextUnlock(parent);
	}
	(*context->methods->delete) (context);
	gtm_pfree(context);
}

void
MemoryContextDelete(MemoryContext context)
{
	MemoryContextDeleteInternal(context, false);
}

/*
 * Gtm_MemoryContextDeleteChildren
 *		Delete all the descendants of the named context and release all
 *		space allocated therein.  The named context itself is not touched.
 */
void
Gtm_MemoryContextDeleteChildren(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	if (MemoryContextIsShared(context))
		MemoryContextLock(context);
	/*
	 * MemoryContextDelete will delink the child from me, so just iterate as
	 * long as there is a child.
	 *
	 * Since the parent is already locked, pass that information to the child
	 * which would then not attempt to relock the parent
	 */
	while (context->firstchild != NULL)
		MemoryContextDeleteInternal(context->firstchild, true);

	if (MemoryContextIsShared(context))
		MemoryContextUnlock(context);
}

/*
 * Gtm_MemoryContextResetAndDeleteChildren
 *		Release all space allocated within a context and delete all
 *		its descendants.
 *
 * This is a common combination case where we want to preserve the
 * specific context but get rid of absolutely everything under it.
 */
void
Gtm_MemoryContextResetAndDeleteChildren(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	Gtm_MemoryContextDeleteChildren(context);
	(*context->methods->reset) (context);
}

/*
 * Gtm_GetMemoryChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 *
 * This is useful for measuring the total space occupied by a set of
 * allocated chunks.
 */
Size
Gtm_GetMemoryChunkSpace(void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->context));

	return (*header->context->methods->get_chunk_space) (header->context,
														 pointer);
}

/*
 * GetMemoryChunkContext
 *		Given a currently-allocated chunk, determine the context
 *		it belongs to.
 */
MemoryContext
GetMemoryChunkContext(void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->context));

	return header->context;
}

/*
 * Gtm_MemoryContextIsEmpty
 *		Is a memory context empty of any allocated space?
 */
bool
Gtm_MemoryContextIsEmpty(MemoryContext context)
{
	AssertArg(MemoryContextIsValid(context));

	/*
	 * For now, we consider a memory context nonempty if it has any children;
	 * perhaps this should be changed later.
	 */
	if (context->firstchild != NULL)
		return false;
	/* Otherwise use the type-specific inquiry */
	return (*context->methods->is_empty) (context);
}

/*
 * Gtm_MemoryContextStats
 *		Print statistics about the named context and all its descendants.
 *
 * This is just a debugging utility, so it's not fancy.  The statistics
 * are merely sent to stderr.
 */
void
Gtm_MemoryContextStats(MemoryContext context)
{
	Gtm_MemoryContextStatsInternal(context, 0);
}

static void
Gtm_MemoryContextStatsInternal(MemoryContext context, int level)
{
	MemoryContext child;

	AssertArg(MemoryContextIsValid(context));

	(*context->methods->stats) (context, level);
	for (child = context->firstchild; child != NULL; child = child->nextchild)
		Gtm_MemoryContextStatsInternal(child, level + 1);
}

/*
 * Gtm_MemoryContextCheck
 *		Check all chunks in the named context.
 *
 * This is just a debugging utility, so it's not fancy.
 */
#ifdef MEMORY_CONTEXT_CHECKING
void
Gtm_MemoryContextCheck(MemoryContext context)
{
	MemoryContext child;

	AssertArg(MemoryContextIsValid(context));

	(*context->methods->check) (context);
	for (child = context->firstchild; child != NULL; child = child->nextchild)
		Gtm_MemoryContextCheck(child);
}
#endif

/*
 * Gtm_MemoryContextContains
 *		Detect whether an allocated chunk of memory belongs to a given
 *		context or not.
 *
 * Caution: this test is reliable as long as 'pointer' does point to
 * a chunk of memory allocated from *some* context.  If 'pointer' points
 * at memory obtained in some other way, there is a small chance of a
 * false-positive result, since the bits right before it might look like
 * a valid chunk header by chance.
 */
bool
Gtm_MemoryContextContains(MemoryContext context, void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	if (pointer == NULL || pointer != (void *) MAXALIGN(pointer))
		return false;

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	/*
	 * If the context link doesn't match then we certainly have a non-member
	 * chunk.  Also check for a reasonable-looking size as extra guard against
	 * being fooled by bogus pointers.
	 */
	if (header->context == context && AllocSizeIsValid(header->size))
		return true;
	return false;
}

/*--------------------
 * Gtm_MemoryContextCreate
 *		Context-type-independent part of context creation.
 *
 * This is only intended to be called by context-type-specific
 * context creation routines, not by the unwashed masses.
 *
 * The context creation procedure is a little bit tricky because
 * we want to be sure that we don't leave the context tree invalid
 * in case of failure (such as insufficient memory to allocate the
 * context node itself).  The procedure goes like this:
 *	1.	Context-type-specific routine first calls Gtm_MemoryContextCreate(),
 *		passing the appropriate tag/size/methods values (the methods
 *		pointer will ordinarily point to statically allocated data).
 *		The parent and name parameters usually come from the caller.
 *	2.	Gtm_MemoryContextCreate() attempts to allocate the context node,
 *		plus space for the name.  If this fails we can ereport() with no
 *		damage done.
 *	3.	We fill in all of the type-independent MemoryContext fields.
 *	4.	We call the type-specific init routine (using the methods pointer).
 *		The init routine is required to make the node minimally valid
 *		with zero chance of failure --- it can't allocate more memory,
 *		for example.
 *	5.	Now we have a minimally valid node that can behave correctly
 *		when told to reset or delete itself.  We link the node to its
 *		parent (if any), making the node part of the context tree.
 *	6.	We return to the context-type-specific routine, which finishes
 *		up type-specific initialization.  This routine can now do things
 *		that might fail (like allocate more memory), so long as it's
 *		sure the node is left in a state that delete will handle.
 *
 * This protocol doesn't prevent us from leaking memory if step 6 fails
 * during creation of a top-level context, since there's no parent link
 * in that case.  However, if you run out of memory while you're building
 * a top-level context, you might as well go home anyway...
 *
 * Normally, the context node and the name are allocated from
 * TopMemoryContext (NOT from the parent context, since the node must
 * survive resets of its parent context!).	However, this routine is itself
 * used to create TopMemoryContext!  If we see that TopMemoryContext is NULL,
 * we assume we are creating TopMemoryContext and use malloc() to allocate
 * the node.
 *
 * Note that the name field of a MemoryContext does not point to
 * separately-allocated storage, so it should not be freed at context
 * deletion.
 *--------------------
 */
MemoryContext
Gtm_MemoryContextCreate(Size size,
					MemoryContextMethods *methods,
					MemoryContext parent,
					const char *name)
{
	MemoryContext node;
	Size		needed = size + strlen(name) + 1;


	/* Get space for node and name */
	if (TopMemoryContext != NULL)
	{
		/* Normal case: allocate the node in TopMemoryContext */
		node = (MemoryContext) Gtm_MemoryContextAlloc(TopMemoryContext,
												  needed);
	}
	else
	{
		/* Special case for startup: use good ol' malloc */
		node = (MemoryContext) malloc(needed);
		Assert(node != NULL);
	}

	/* Initialize the node as best we can */
	MemSet(node, 0, size);
	node->methods = methods;
	node->parent = NULL;		/* for the moment */
	node->firstchild = NULL;
	node->nextchild = NULL;
	node->name = ((char *) node) + size;
	strcpy(node->name, name);

	/* Type-specific routine finishes any other essential initialization */
	(*node->methods->init) (node);

	/*
	 * Lock the parent context if the it is shared and must be made thread-safe
	 */
	if ((parent != NULL) && (MemoryContextIsShared(parent)))
		MemoryContextLock(parent);

	/* OK to link node to parent (if any) */
	if (parent)
	{
		node->parent = parent;
		node->nextchild = parent->firstchild;
		parent->firstchild = node;
	}

	if ((parent != NULL) && (MemoryContextIsShared(parent)))
		MemoryContextUnlock(parent);

	/* Return to type-specific creation routine to finish up */
	return node;
}

/*
 * Gtm_MemoryContextAlloc
 *		Allocate space within the specified context.
 *
 * This could be turned into a macro, but we'd have to import
 * nodes/memnodes.h into postgres.h which seems a bad idea.
 */
void *
Gtm_MemoryContextAlloc(MemoryContext context, Size size)
{
	AssertArg(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %lu",
			 (unsigned long) size);

	return (*context->methods->alloc) (context, size);
}

/*
 * Gtm_Gtm_MemoryContextAllocZero
 *		Like Gtm_MemoryContextAlloc, but clears allocated memory
 *
 *	We could just call Gtm_MemoryContextAlloc then clear the memory, but this
 *	is a very common combination, so we provide the combined operation.
 */
void *
Gtm_Gtm_MemoryContextAllocZero(MemoryContext context, Size size)
{
	void	   *ret;

	AssertArg(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %lu",
			 (unsigned long) size);

	ret = (*context->methods->alloc) (context, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * Gtm_Gtm_Gtm_MemoryContextAllocZeroAligned
 *		Gtm_Gtm_MemoryContextAllocZero where length is suitable for MemSetLoop
 *
 *	This might seem overly specialized, but it's not because newNode()
 *	is so often called with compile-time-constant sizes.
 */
void *
Gtm_Gtm_Gtm_MemoryContextAllocZeroAligned(MemoryContext context, Size size)
{
	void	   *ret;

	AssertArg(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %lu",
			 (unsigned long) size);

	ret = (*context->methods->alloc) (context, size);

	MemSetLoop(ret, 0, size);

	return ret;
}

/*
 * gtm_pfree
 *		Release an allocated chunk.
 */
void
gtm_pfree(void *pointer)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->context));

	(*header->context->methods->free_p) (header->context, pointer);
}

/*
 * gtm_repalloc
 *		Adjust the size of a previously allocated chunk.
 */
void *
gtm_repalloc(void *pointer, Size size)
{
	StandardChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	/*
	 * OK, it's probably safe to look at the chunk header.
	 */
	header = (StandardChunkHeader *)
		((char *) pointer - STANDARDCHUNKHEADERSIZE);

	AssertArg(MemoryContextIsValid(header->context));

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %lu",
			 (unsigned long) size);

	return (*header->context->methods->realloc) (header->context,
												 pointer, size);
}

/*
 * MemoryContextSwitchTo
 *		Returns the current context; installs the given context.
 *
 * This is inlined when using GCC.
 *
 * TODO: investigate supporting inlining for some non-GCC compilers.
 */
MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
	MemoryContext old;

	AssertArg(MemoryContextIsValid(context));

	old = CurrentMemoryContext;
	CurrentMemoryContext = context;
	return old;
}

/*
 * Gtm_MemoryContextStrdup
 *		Like strdup(), but allocate from the specified context
 */
char *
Gtm_MemoryContextStrdup(MemoryContext context, const char *string)
{
	char	   *nstr;
	Size		len = strlen(string) + 1;

	nstr = (char *) Gtm_MemoryContextAlloc(context, len);

	memcpy(nstr, string, len);

	return nstr;
}

/*
 * gtm_pnstrdup
 *		Like pstrdup(), but append null byte to a
 *		not-necessarily-null-terminated input string.
 */
char *
gtm_pnstrdup(const char *in, Size len)
{
	char	   *out = palloc(len + 1);

	memcpy(out, in, len);
	out[len] = '\0';
	return out;
}


#if defined(WIN32) || defined(__CYGWIN__)
/*
 *	Memory support routines for libpgport on Win32
 *
 *	Win32 can't load a library that PGDLLIMPORTs a variable
 *	if the link object files also PGDLLIMPORT the same variable.
 *	For this reason, libpgport can't reference CurrentMemoryContext
 *	in the palloc macro calls.
 *
 *	To fix this, we create several functions here that allow us to
 *	manage memory without doing the inline in libpgport.
 */
void *
pgport_palloc(Size sz)
{
	return palloc(sz);
}


char *
pgport_pstrdup(const char *str)
{
	return pstrdup(str);
}


/* Doesn't reference a PGDLLIMPORT variable, but here for completeness. */
void
pgport_gtm_pfree(void *pointer)
{
	gtm_pfree(pointer);
}


#endif

#include "gen_alloc.h"
void *current_memcontext(void);
void *current_memcontext(void)
{
	return((void *)CurrentMemoryContext);
}

void *allocTopMemCxt(size_t s)
{
	return (void *)Gtm_MemoryContextAlloc(TopMostMemoryContext, (Size)s);
}

Gen_Alloc genAlloc_class = {(void *)Gtm_MemoryContextAlloc,
                            (void *)Gtm_Gtm_MemoryContextAllocZero,
                            (void *)gtm_repalloc,
                            (void *)gtm_pfree,
                            (void *)current_memcontext,
							(void *)allocTopMemCxt};