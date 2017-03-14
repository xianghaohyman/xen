(*
 * Copyright (C) 2006-2007 XenSource Ltd.
 * Copyright (C) 2008      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *)

let debug fmt = Logging.debug "domains" fmt
let error fmt = Logging.error "domains" fmt
let warn fmt  = Logging.warn  "domains" fmt

type domains = {
	eventchn: Event.t;
	table: (Xenctrl.domid, Domain.t) Hashtbl.t;

	(* N.B. the Queue module is not thread-safe but oxenstored is single-threaded. *)
	(* Domains queue up to regain conflict-credit; we have a queue for
	   domains that are carrying some penalty and so are below the
	   maximum credit, and another queue for domains that have run out of
	   credit and so have had their access paused. *)
	doms_conflict_paused: (Domain.t option ref) Queue.t;
	doms_with_conflict_penalty: (Domain.t option ref) Queue.t;

	(* A callback function to be called when we go from zero to one paused domain.
	   This will be to reset the countdown until the next unit of credit is issued. *)
	on_first_conflict_pause: unit -> unit;

	(* If config is set to use individual instead of aggregate conflict-rate-limiting,
	   we use this instead of the queues. *)
	mutable n_paused: int;
}

let init eventchn = {
	eventchn = eventchn;
	table = Hashtbl.create 10;
	doms_conflict_paused = Queue.create ();
	doms_with_conflict_penalty = Queue.create ();
	on_first_conflict_pause = (fun () -> ()); (* Dummy value for now, pending subsequent commit. *)
	n_paused = 0;
}
let del doms id = Hashtbl.remove doms.table id
let exist doms id = Hashtbl.mem doms.table id
let find doms id = Hashtbl.find doms.table id
let number doms = Hashtbl.length doms.table
let iter doms fct = Hashtbl.iter (fun _ b -> fct b) doms.table

(* Functions to handle queues of domains given that the domain might be deleted while in a queue. *)
let push dom queue =
	Queue.push (ref (Some dom)) queue

let rec pop queue =
	match !(Queue.pop queue) with
	| None -> pop queue
	| Some x -> x

let remove_from_queue dom queue =
	Queue.iter (fun d -> match !d with
		| None -> ()
		| Some x -> if x=dom then d := None) queue

let cleanup xc doms =
	let notify = ref false in
	let dead_dom = ref [] in

	Hashtbl.iter (fun id _ -> if id <> 0 then
		try
			let info = Xenctrl.domain_getinfo xc id in
			if info.Xenctrl.shutdown || info.Xenctrl.dying then (
				debug "Domain %u died (dying=%b, shutdown %b -- code %d)"
				                    id info.Xenctrl.dying info.Xenctrl.shutdown info.Xenctrl.shutdown_code;
				if info.Xenctrl.dying then
					dead_dom := id :: !dead_dom
				else
					notify := true;
			)
		with Xenctrl.Error _ ->
			debug "Domain %u died -- no domain info" id;
			dead_dom := id :: !dead_dom;
		) doms.table;
	List.iter (fun id ->
		let dom = Hashtbl.find doms.table id in
		Domain.close dom;
		Hashtbl.remove doms.table id;
		if dom.Domain.conflict_credit <= !Define.conflict_burst_limit
		then (
			remove_from_queue dom doms.doms_with_conflict_penalty;
			if (dom.Domain.conflict_credit <= 0.) then remove_from_queue dom doms.doms_conflict_paused
		)
	) !dead_dom;
	!notify, !dead_dom

let resume doms domid =
	()

let create xc doms domid mfn port =
	let interface = Xenctrl.map_foreign_range xc domid (Xenmmap.getpagesize()) mfn in
	let dom = Domain.make domid mfn port interface doms.eventchn in
	Hashtbl.add doms.table domid dom;
	Domain.bind_interdomain dom;
	dom

let create0 fake doms =
	let port, interface =
		if fake then (
			0, Xenctrl.with_intf (fun xc -> Xenctrl.map_foreign_range xc 0 (Xenmmap.getpagesize()) 0n)
		) else (
			let port = Utils.read_file_single_integer Define.xenstored_proc_port
			and fd = Unix.openfile Define.xenstored_proc_kva
					       [ Unix.O_RDWR ] 0o600 in
			let interface = Xenmmap.mmap fd Xenmmap.RDWR Xenmmap.SHARED
						  (Xenmmap.getpagesize()) 0 in
			Unix.close fd;
			port, interface
		)
		in
	let dom = Domain.make 0 Nativeint.zero port interface doms.eventchn in
	Hashtbl.add doms.table 0 dom;
	Domain.bind_interdomain dom;
	Domain.notify dom;
	dom

let decr_conflict_credit doms dom =
	let before = dom.Domain.conflict_credit in
	let after = max (-1.0) (before -. 1.0) in
	dom.Domain.conflict_credit <- after;
	if !Define.conflict_rate_limit_is_aggregate then (
		if before >= !Define.conflict_burst_limit
		&& after < !Define.conflict_burst_limit
		&& after > 0.0
		then (
			push dom doms.doms_with_conflict_penalty
		) else if before > 0.0 && after <= 0.0
		then (
			let first_pause = Queue.is_empty doms.doms_conflict_paused in
			push dom doms.doms_conflict_paused;
			if first_pause then doms.on_first_conflict_pause ()
		) else (
			(* The queues are correct already: no further action needed. *)
		)
	) else if before > 0.0 && after <= 0.0 then (
		doms.n_paused <- doms.n_paused + 1;
		if doms.n_paused = 1 then doms.on_first_conflict_pause ()
	)

(* Give one point of credit to one domain, and update the queues appropriately. *)
let incr_conflict_credit_from_queue doms =
	let process_queue q requeue_test =
		let d = pop q in
		d.Domain.conflict_credit <- min (d.Domain.conflict_credit +. 1.0) !Define.conflict_burst_limit;
		if requeue_test d.Domain.conflict_credit then (
			push d q (* Make it queue up again for its next point of credit. *)
		)
	in
	let paused_queue_test cred = cred <= 0.0 in
	let penalty_queue_test cred = cred < !Define.conflict_burst_limit in
	try process_queue doms.doms_conflict_paused paused_queue_test
	with Queue.Empty -> (
		try process_queue doms.doms_with_conflict_penalty penalty_queue_test
		with Queue.Empty -> () (* Both queues are empty: nothing to do here. *)
	)

let incr_conflict_credit doms =
	if !Define.conflict_rate_limit_is_aggregate
	then incr_conflict_credit_from_queue doms
	else (
		(* Give a point of credit to every domain, subject only to the cap. *)
		let inc dom =
			let before = dom.Domain.conflict_credit in
			let after = min (before +. 1.0) !Define.conflict_burst_limit in
			dom.Domain.conflict_credit <- after;
			if before <= 0.0 && after > 0.0
			then doms.n_paused <- doms.n_paused - 1
		in
		(* Scope for optimisation (probably tiny): avoid iteration if all domains are at max credit *)
		iter doms inc
	)
