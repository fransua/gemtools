/*
   GEMTools python binding utilities
   */
#include "gemtools_binding.h"
#include <omp.h>


void gt_stats_fill(gt_input_file* input_file, gt_stats* target_all_stats, gt_stats* target_best_stats, uint64_t num_threads, bool paired_end){
  // Stats info
  gt_stats** all_stats = malloc(num_threads*sizeof(gt_stats*));
  all_stats[0] = target_all_stats;
  gt_stats** best_stats = malloc(num_threads*sizeof(gt_stats*));
  best_stats[0] = target_best_stats;

  gt_stats_analysis params_all = GT_STATS_ANALYSIS_DEFAULT();
  params_all.first_map = false;
  gt_stats_analysis params_best = GT_STATS_ANALYSIS_DEFAULT();
  params_best.first_map = true;

  //params_all.indel_profile = true
  // Parallel reading+process
  #pragma omp parallel num_threads(num_threads)
  {
    uint64_t tid = omp_get_thread_num();
    gt_buffered_input_file* buffered_input = gt_buffered_input_file_new(input_file);

    gt_status error_code;
    gt_template *template = gt_template_new();
    if(tid > 0){
      all_stats[tid] = gt_stats_new();
      best_stats[tid] = gt_stats_new();
    }
    gt_generic_parser_attributes* generic_parser_attr =  gt_input_generic_parser_attributes_new(paired_end);
    while ((error_code=gt_input_generic_parser_get_template(buffered_input,template, generic_parser_attr))) {
      if (error_code!=GT_IMP_OK) {
        gt_error_msg("Fatal error parsing file\n");
      }
      // Extract all_stats
      if(target_all_stats != NULL){
        gt_stats_calculate_template_stats(all_stats[tid],template,NULL, &params_all);
      }
      if(target_best_stats != NULL){
        gt_stats_calculate_template_stats(best_stats[tid],template,NULL, &params_best);
      }
    }
    // Clean
    gt_template_delete(template);
    // gt_template_delete(template_copy);
    gt_buffered_input_file_close(buffered_input);
  }

  // Merge all_stats
  if(target_all_stats != NULL){
    gt_stats_merge(all_stats, num_threads);
  }
  if(target_best_stats != NULL){
    gt_stats_merge(best_stats, num_threads);
  }

  // Clean
  free(all_stats);
  free(best_stats);
  gt_input_file_close(input_file);
}


bool gt_input_file_has_qualities(gt_input_file* file){
  return (file->file_format == FASTA && file->fasta_type.fasta_format == F_FASTQ) || (file->file_format == MAP && file->map_type.contains_qualities);
}

void gt_merge_files_synch(gt_output_file* const output_file, uint64_t threads, const uint64_t num_files,  gt_input_file** files) {
  // Mutex
  pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
  // Parallel reading+process
#pragma omp parallel num_threads(threads)
  {
    //gt_merge_map_files(&input_mutex,input_file_1,input_file_2,false, same_content ,output_file);
    gt_merge_synch_map_files_a(&input_mutex, false, output_file, files, num_files);
  }
}

void gt_write_stream(gt_output_file* output, gt_input_file** inputs, uint64_t num_inputs, bool append_extra, bool clean_id, bool interleave, uint64_t threads, bool write_map, bool remove_scores){
  // prepare attributes

  gt_output_fasta_attributes* attributes = 0;
  gt_output_map_attributes* map_attributes = 0;
  if(!write_map){
    attributes = gt_output_fasta_attributes_new();
    gt_output_fasta_attributes_set_print_extra(attributes, append_extra);
    gt_output_fasta_attributes_set_print_casava(attributes, !clean_id);
    // check qualities
    if(!gt_input_file_has_qualities(inputs[0])){
      gt_output_fasta_attributes_set_format(attributes, F_FASTA);
    }
  }else{

    map_attributes = gt_output_map_attributes_new();
    gt_output_map_attributes_set_print_extra(map_attributes, append_extra);
    gt_output_map_attributes_set_print_casava(map_attributes, !clean_id);
    gt_output_map_attributes_set_print_scores(map_attributes, !remove_scores);
  }

  // generic parser attributes
  gt_generic_parser_attributes* parser_attributes = gt_input_generic_parser_attributes_new(false); // do not force pairs
  pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

  if(interleave){
    // main loop, interleave
    #pragma omp parallel num_threads(threads)
    {
      register uint64_t i = 0;
      register uint64_t c = 0;
      gt_buffered_output_file* buffered_output = gt_buffered_output_file_new(output);
      gt_buffered_input_file** buffered_input = malloc(num_inputs * sizeof(gt_buffered_input_file*));

      for(i=0; i<num_inputs; i++){
        buffered_input[i] = gt_buffered_input_file_new(inputs[i]);
      }
      // attache first input to output
      gt_buffered_input_file_attach_buffered_output(buffered_input[0], buffered_output);

      gt_template* template = gt_template_new();
      gt_status status;
      i=0;
      while( gt_input_generic_parser_synch_blocks_a(&input_mutex, buffered_input, num_inputs, parser_attributes) == GT_STATUS_OK ){
        for(i=0; i<num_inputs; i++){
          if( (status = gt_input_generic_parser_get_template(buffered_input[i], template, parser_attributes)) == GT_STATUS_OK){
            if(write_map){
              gt_output_map_bofprint_template(buffered_output, template, map_attributes);
            }else{
              gt_output_fasta_bofprint_template(buffered_output, template, attributes);
            }
            c++;
          }
        }
      }
      gt_buffered_output_file_close(buffered_output);
      for(i=0; i<num_inputs; i++){
        gt_buffered_input_file_close(buffered_input[i]);
      }
      gt_template_delete(template);
      free(buffered_input);
    }
  }else{
    // main loop, cat
    #pragma omp parallel num_threads(threads)
    {
      register uint64_t i = 0;
      register uint64_t c = 0;
      register uint64_t last_id = 0;
      gt_buffered_output_file* buffered_output = gt_buffered_output_file_new(output);
      gt_buffered_input_file** buffered_input = malloc(1 * sizeof(gt_buffered_input_file*));
      gt_buffered_input_file* current_input = 0;
      gt_template* template = gt_template_new();
      gt_status status = 0;
      for(i=0; i<num_inputs;i++){
        // create input buffer
        if(i>0){
          gt_buffered_input_file_close(current_input);
          inputs[i]->processed_id = last_id;
        }
        current_input = gt_buffered_input_file_new(inputs[i]);
        buffered_input[0] = current_input;
        // attache the buffer
        gt_buffered_input_file_attach_buffered_output(current_input, buffered_output);

        // read
        while( gt_input_generic_parser_synch_blocks_a(&input_mutex, buffered_input, 1, parser_attributes) == GT_STATUS_OK ){
          if( (status = gt_input_generic_parser_get_template(current_input, template, parser_attributes)) == GT_STATUS_OK){
            if(write_map){
              gt_output_map_bofprint_template(buffered_output, template, map_attributes);
            }else{
              gt_output_fasta_bofprint_template(buffered_output, template, attributes);
            }
            c++;
          }
        }
        last_id = inputs[i]->processed_id;
      }
      gt_buffered_input_file_close(current_input);
      gt_buffered_output_file_close(buffered_output);
      gt_template_delete(template);
      free(buffered_input);
    }

  }
  if(attributes != NULL) gt_output_fasta_attributes_delete(attributes);
  if(map_attributes != NULL)gt_output_map_attributes_delete(map_attributes);
  gt_input_generic_parser_attributes_delete(parser_attributes);

  // register uint64_t i = 0;
  // for(i=0; i<num_inputs; i++){
  //     gt_input_file_close(inputs[i]);
  // }
  // gt_output_file_close(output);
}

void gt_stats_print_stats(FILE* output, gt_stats* const stats, const bool paired_end) {
  register uint64_t num_reads = stats->num_blocks;
  /*
   * General.Stats (Reads,Alignments,...)
   */
  fprintf(output,"[GENERAL.STATS]\n");
  gt_stats_print_general_stats(output,stats,num_reads,paired_end);
  /*
   * Maps
   */
  // if (parameters.maps_profile) {
  fprintf(output,"[MAPS.PROFILE]\n");
  gt_stats_print_maps_stats(output, stats,num_reads,paired_end);
  // }
  if (paired_end) {
    gt_stats_print_inss_distribution(output,stats->maps_profile->inss,stats->num_maps, true);
  }
  /*
   * Print Quality Scores vs Errors/Misms
   */
  // if (parameters.mismatch_quality) {
  {
    register const gt_maps_profile* const maps_profile = stats->maps_profile;
    if (maps_profile->total_mismatches > 0) {
      fprintf(output,"[MISMATCH.QUALITY]\n");
      gt_stats_print_qualities_error_distribution(
          output,maps_profile->qual_score_misms,maps_profile->total_mismatches);
    }
    if (maps_profile->total_errors_events > 0) {
      fprintf(output,"[ERRORS.QUALITY]\n");
      gt_stats_print_qualities_error_distribution(
          output,maps_profile->qual_score_errors,maps_profile->total_errors_events);
    }
  }
  // }
  /*
   * Print Mismatch transition table
   */
  // if (parameters.mismatch_transitions) {
  {
    register const gt_maps_profile* const maps_profile = stats->maps_profile;
    if (maps_profile->total_mismatches > 0) {
      fprintf(output,"[MISMATCH.TRANSITIONS]\n");
      fprintf(output,"MismsTransitions\n");
      gt_stats_print_misms_transition_table(
          output,maps_profile->misms_transition,maps_profile->total_mismatches);
      fprintf(output,"MismsTransitions.1-Nucleotide.Context");
      gt_stats_print_misms_transition_table_1context(
          output,maps_profile->misms_1context,maps_profile->total_mismatches);
    }
  }
  // }
  /*
   * Print Splitmaps profile
   */
  // if (parameters.splitmaps_profile) {
  fprintf(output,"[SPLITMAPS.PROFILE]\n");
  gt_stats_print_split_maps_stats(output,stats, paired_end);
  // }
}

void gt_score_filter(gt_template* template_dst,gt_template* template_src, gt_filter_params* params) {
  bool is_4 = false;
  bool best_printed = false;
  GT_TEMPLATE_IF_REDUCES_TO_ALINGMENT(template_src,alignment_src) {
    GT_TEMPLATE_REDUCTION(template_dst,alignment_dst);
    GT_ALIGNMENT_ITERATE(alignment_src,map) {
      const int64_t score = get_mapq(map->gt_score);
      if(   (params->group_1 && 252 <= score && score <= 254)
          ||	(params->group_2 && 177 <= score && score <= 180)
          ||	(params->group_3 && 123 <= score && score <= 127)
          ||	(params->group_4 && (  (114 <= score && score <= 119)
              ||  (95  <= score && score <= 110 && is_4)))

        ) {
        if (!is_4 && 114 <= score && score <= 119){
          is_4= true;
        }
        if(!best_printed) gt_alignment_insert_map(alignment_dst, gt_map_copy(map));
        if(score > 119){
          best_printed = true;
        }
      }
    }
  } GT_TEMPLATE_END_REDUCTION__RETURN;

  const uint64_t num_blocks = gt_template_get_num_blocks(template_src);
  GT_TEMPLATE_ITERATE_MMAP__ATTR(template_src,mmap,mmap_attributes) {
    const int64_t score = get_mapq(mmap_attributes->gt_score);

    if(   (params->group_1 && 252 <= score && score <= 254)
        ||	(params->group_2 && 177 <= score && score <= 180)
        ||	(params->group_3 && 123 <= score && score <= 127)
        ||	(params->group_4 && (  (114 <= score && score <= 119)
            ||  (95  <= score && score <= 110 && is_4)))

      ) {
      if (!is_4 && 114 <= score && score <= 119){
        is_4= true;
      }
      if(!best_printed){
        gt_map** mmap_copy = gt_mmap_array_copy(mmap,__mmap_num_blocks);
        gt_template_insert_mmap(template_dst, mmap_copy, mmap_attributes);
      }
      if(score > 119){
        best_printed = true;
      }
    }
  }
}



void gt_annotation_filter(gt_template* template_dst,gt_template* template_src, gt_filter_params* params, gt_gtf* gtf) {
  register const uint64_t num_blocks = gt_template_get_num_blocks(template_src);
  gt_gtf_hits* hits = gt_gtf_hits_new();
  gt_gtf_search_template_for_exons(gtf, hits, template_src);
  uint64_t i = 0;

  GT_TEMPLATE_IF_REDUCES_TO_ALINGMENT(template_src, alignment_src) {
    GT_TEMPLATE_REDUCTION(template_dst,alignment_dst);
    GT_ALIGNMENT_ITERATE(alignment_src,map) {
      gt_string* gene_id = *gt_vector_get_elm(hits->ids, i, gt_string*);
      if(gene_id != NULL){
        printf("Annotation mapped : %s -> Overlap: %f Distance:%d LV: %d Score: %d Type:%s Exonic:%d\n",
            gt_string_get_string(gene_id),
            *gt_vector_get_elm(hits->scores, i, float),
            gt_map_get_segment_distance(map),
            gt_map_get_global_levenshtein_distance(map),
            gt_map_get_score(map),
            gt_string_get_string(*gt_vector_get_elm(hits->types, i, gt_string*)),
            *gt_vector_get_elm(hits->exonic, i, bool)
            );
      }else{
        printf("Annotation mapped : %s -> Overlap: %f Distance:%d LV: %d Score: %d\n",
            "NULL",
            *gt_vector_get_elm(hits->scores, i, float),
            gt_map_get_segment_distance(map),
            gt_map_get_global_levenshtein_distance(map),
            gt_map_get_score(map)
            );
      }
      i++;
      gt_alignment_insert_map(alignment_dst,gt_map_copy(map));
    }
  } GT_TEMPLATE_END_REDUCTION__RETURN;

  GT_TEMPLATE_ITERATE_MMAP__ATTR(template_src,mmap,mmap_attr) {
    gt_string* gene_id = *gt_vector_get_elm(hits->ids, i, gt_string*);
    if(gene_id != NULL){
      printf("Annotation mapped : %s -> Overlap: %f Distance:%d LV: %d Score: %d Type:%s Exonic:%d\n",
          gt_string_get_string(gene_id),
          *gt_vector_get_elm(hits->scores, i, float),
          mmap_attr->distance,
          gt_mmap_get_global_levenshtein_distance(mmap, num_blocks),
          get_mapq(mmap_attr->gt_score),
          gt_string_get_string(*gt_vector_get_elm(hits->types, i, gt_string*)),
          *gt_vector_get_elm(hits->exonic, i, bool)
          );
    }else{
      printf("Annotation mapped : %s -> Overlap: %f Distance:%d LV: %d Score: %d\n",
          "NULL",
          *gt_vector_get_elm(hits->scores, i, float),
          mmap_attr->distance,
          gt_mmap_get_global_levenshtein_distance(mmap, num_blocks),
          get_mapq(mmap_attr->gt_score)
          );
    }
    i++;
    register gt_map** mmap_copy = gt_mmap_array_copy(mmap,num_blocks);
    gt_template_insert_mmap(template_dst,mmap_copy,mmap_attr);
    free(mmap_copy);
  }
  gt_gtf_hits_delete(hits);
}

GT_INLINE void gt_alignment_recalculate_counters_no_splits(gt_alignment* const alignment) {
  GT_ALIGNMENT_CHECK(alignment);
  gt_vector_clear(gt_alignment_get_counters_vector(alignment));
  // Recalculate counters
  gt_alignment_map_iterator map_iterator;
  gt_alignment_new_map_iterator(alignment,&map_iterator);
  gt_map* map;
  while ((map=gt_alignment_next_map(&map_iterator))!=NULL) {
    gt_alignment_inc_counter(alignment, gt_map_get_no_split_distance(map));
  }
}

GT_INLINE uint64_t gt_map_get_no_split_distance(gt_map* const map){
  return gt_map_get_global_distance(map) - (gt_map_get_num_blocks(map)-1);
}

GT_INLINE uint64_t gt_template_get_no_split_distance(gt_template* const template, gt_map** mmap, const uint64_t num_blocks){
  uint64_t i, total_distance = 0;
  for (i=0;i<num_blocks;++i) {
    total_distance+= gt_map_get_no_split_distance(mmap[i]);//(gt_map_get_global_distance(mmap[i]) - gt_map_get_num_blocks(mmap[i]));
  }
  return total_distance;
}

GT_INLINE void gt_template_recalculate_counters_no_splits(gt_template* const template) {
  GT_TEMPLATE_CHECK(template);
  GT_TEMPLATE_IF_REDUCES_TO_ALINGMENT(template,alignment) {
    gt_alignment_recalculate_counters_no_splits(alignment);
  } GT_TEMPLATE_END_REDUCTION__RETURN;
  // Clear previous counters
  gt_vector_clear(gt_template_get_counters_vector(template));
  // Recalculate counters
  const uint64_t num_blocks = gt_template_get_num_blocks(template);
  GT_TEMPLATE_ITERATE_MMAP__ATTR_(template,mmap,mmap_attr) {
    uint64_t total_distance = gt_template_get_no_split_distance(template, mmap, num_blocks);
    mmap_attr->distance=total_distance;
    gt_template_inc_counter(template,total_distance);
  }
}


bool gt_filter_is_group_1_template(gt_template* template, gt_map** mmap, gt_mmap_attributes* attributes){
  // filter for group one maps
  // maps that have only one mapping
  return gt_template_get_num_mmaps(template) == 1;
}


void gt_template_filter(gt_template* template_dst,gt_template* template_src, gt_gtf* gtf, gt_filter_params* params) {
  bool is_4 = false;
  bool best_printed = false;
  /*SE*/
  GT_TEMPLATE_IF_REDUCES_TO_ALINGMENT(template_src,alignment_src) {
    GT_TEMPLATE_REDUCTION(template_dst,alignment_dst);
    GT_ALIGNMENT_ITERATE(alignment_src,map) {
      if(params->min_score > 0){
        const int64_t score = get_mapq(map->gt_score);
        if(score < params->min_score) continue;
      }

      // Check SM contained
      const uint64_t total_distance = gt_map_get_no_split_distance(map); //gt_map_get_global_distance(map);
      const uint64_t lev_distance = gt_map_get_global_levenshtein_distance(map);
      if (params->min_event_distance > total_distance || total_distance > params->max_event_distance) continue;
      if (params->min_levenshtein_distance > lev_distance || lev_distance > params->max_levenshtein_distance) continue;

      //printf("Check intron size ...%d\n", params->min_intron_length);
      if(params->min_intron_length > 0){
        // ignore mapping if it contains splits and their length is < min_intron_length
        if(gt_map_get_num_blocks(map) > 1){
          uint64_t min_intron_length = UINT64_MAX;
          GT_MAP_ITERATE(map, map_block){
            if(gt_map_has_next_block(map_block)){
              uint64_t l = gt_map_get_junction_size(map_block);
              if(min_intron_length > l){
                min_intron_length = l;
              }
            }
          }
          printf("found intron length %d\n", min_intron_length);
          if(min_intron_length < params->min_intron_length){
            continue;
          }
        }
      }
      if(params->min_block_length > 0){
        // ignore mapping if its a splitmap and the block length is < min_block_length
        if(gt_map_get_num_blocks(map) > 1){
          uint64_t min_block_length = UINT64_MAX;
          GT_MAP_ITERATE(map, map_block){
            uint64_t l = gt_map_get_base_length(map);
            if(min_block_length > l){
              min_block_length = l;
            }
          }
          if(min_block_length < params->min_block_length){
            continue;
          }
        }
      }

//      // bad quality mismatchs
//      if(params->quality_offset > 0){
//        GT_MISMS_ITERATE(map, misms){
//          if(misms->misms_type == MISMS){
//            uint8_t qv = gt_alignment_get_qualities(alignment_src)[misms->position] - params->quality_offset;
//          }
//        }
//      }

      if(params->min_unique_level >= 0){
        int64_t level = gt_alignment_get_uniq_degree(alignment_src);
        if(level >= 0 && params->min_unique_level <= level){
          // insert the first alignment and continue
          gt_alignment_insert_map(alignment_dst, gt_map_copy(map));
          break;
        }
      }
      // Insert the map
      gt_alignment_insert_map(alignment_dst, gt_map_copy(map));
    }
  } GT_TEMPLATE_END_REDUCTION__RETURN;

  /*
   * PE
   */
  GT_TEMPLATE_ITERATE_MMAP__ATTR(template_src,mmap,mmap_attr) {
    if(params->min_score > 0){
      const int64_t score = get_mapq(mmap_attr->gt_score);
      if(score < params->min_score) continue;
    }
    // Check strata
    const uint64_t total_distance = gt_map_get_no_split_distance(mmap[0])+gt_map_get_no_split_distance(mmap[1]);
    if (params->min_event_distance > total_distance || total_distance > params->max_event_distance) continue;

    // Check levenshtein distance
    const uint64_t lev_distance = gt_map_get_global_levenshtein_distance(mmap[0])+gt_map_get_global_levenshtein_distance(mmap[1]);
    if (params->min_levenshtein_distance > lev_distance || lev_distance > params->max_levenshtein_distance) continue;

    // Check inss
    if (params->min_inss > INT64_MIN || params->max_inss < INT64_MAX) {
      gt_status gt_err;
      const int64_t inss = gt_template_get_insert_size(mmap, &gt_err);
      if (params->min_inss > inss || inss > params->max_inss) continue;
    }
    // Check strandness
    if (params->filter_by_strand) {
      if (mmap[0]->strand==FORWARD && mmap[1]->strand==FORWARD) continue;
      if (mmap[0]->strand==REVERSE && mmap[1]->strand==REVERSE) continue;
    }


    // filter by score
    if(params->filter_groups){
      const int64_t score = get_mapq(mmap_attr->gt_score);
      if(   (params->group_1 && 252 <= score && score <= 254)
          ||  (params->group_2 && 177 <= score && score <= 180)
          ||  (params->group_3 && 123 <= score && score <= 127)
          ||  (params->group_4 && (  (114 <= score && score <= 119)
              ||  (95  <= score && score <= 110 && is_4)))

        ) {
        if (!is_4 && 114 <= score && score <= 119){
          is_4= true;
        }
        if(best_printed){
          continue;
        }else{
          if(score > 119){
            best_printed = true;
          }
        }
      }else{
        continue;
      }
    }

    // Add the mmap
    gt_map** mmap_copy = gt_mmap_array_copy(mmap,__mmap_num_blocks);
    gt_template_insert_mmap(template_dst, mmap_copy, mmap_attr);
    gt_free(mmap_copy);
  }
}


int gt_alignment_cmp_distance__score_no_split(gt_map** const map_a,gt_map** const map_b) {
  // Sort by distance
  const int64_t distance_a = gt_map_get_no_split_distance(*map_a);
  const int64_t distance_b = gt_map_get_no_split_distance(*map_b);

  if (distance_a != distance_b) return distance_a-distance_b;

  // Sort by score (here we cannot do the trick as gt_score fills the whole uint64_t range)
  const uint64_t score_a = (*map_a)->gt_score;
  const uint64_t score_b = (*map_b)->gt_score;
  return (score_a > score_b) ? -1 : (score_a < score_b ? 1 : 0);
}

GT_INLINE void gt_alignment_sort_by_distance__score_no_split(gt_alignment* const alignment) {
  GT_ALIGNMENT_CHECK(alignment);
  qsort(gt_vector_get_mem(alignment->maps,gt_map*),gt_vector_get_used(alignment->maps),
      sizeof(gt_map*),(int (*)(const void *,const void *))gt_alignment_cmp_distance__score_no_split);
}

int gt_mmap_cmp_distance__score_no_split(gt_mmap* const mmap_a,gt_mmap* const mmap_b) {
  // Sort by distance
  const int64_t distance_a = mmap_a->attributes.distance;
  const int64_t distance_b = mmap_b->attributes.distance;
  if (distance_a != distance_b) return distance_a-distance_b;
  // Sort by score (here we cannot do the trick as gt_score fills the whole uint64_t range)
  const uint64_t score_a = mmap_a->attributes.gt_score;
  const uint64_t score_b = mmap_b->attributes.gt_score;
  return (score_a > score_b) ? -1 : (score_a < score_b ? 1 : 0);
}


GT_INLINE void gt_template_sort_by_distance__score_no_split(gt_template* const template) {
  GT_TEMPLATE_CHECK(template);
  GT_TEMPLATE_IF_REDUCES_TO_ALINGMENT(template,alignment) {
    return gt_alignment_sort_by_distance__score_no_split(alignment);
  } GT_TEMPLATE_END_REDUCTION;
  // Sort
  const uint64_t num_mmap = gt_template_get_num_mmaps(template);
  qsort(gt_vector_get_mem(template->mmaps,gt_mmap),num_mmap,sizeof(gt_mmap),
      (int (*)(const void *,const void *))gt_mmap_cmp_distance__score_no_split);
}


void gt_filter_stream(gt_input_file* input, gt_output_file* output, uint64_t threads, gt_filter_params* params){
  gt_handle_error_signals();
  gt_gtf* gtf = NULL;
  if(params->annotation != NULL && strlen(params->annotation) > 0){
    FILE* of = fopen(params->annotation, "r");
    if(of == NULL){
      printf("ERROR opening annotation !\n");
      return;
    }
    gtf = gt_gtf_read(of);
    fclose(of);
  }
  if(params->max_matches == 0){
    params->max_matches = UINT64_MAX;
  }
  pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

  // main loop, cat
  #pragma omp parallel num_threads(threads)
  {

    gt_buffered_input_file* buffered_input = gt_buffered_input_file_new(input);
    gt_buffered_output_file* buffered_output = gt_buffered_output_file_new(output);
    gt_buffered_input_file_attach_buffered_output(buffered_input,buffered_output);

    gt_generic_printer_attributes* generic_printer_attributes = gt_generic_printer_attributes_new(MAP);
    gt_output_map_attributes_reset_defaults(generic_printer_attributes->output_map_attributes);
    gt_generic_parser_attributes* generic_parser_attributes = gt_input_generic_parser_attributes_new(params->paired);
    gt_input_map_parser_attributes_set_max_parsed_maps(generic_parser_attributes->map_parser_attributes, params->max_matches); // Limit max-matches

    gt_template* template = gt_template_new();
    gt_status status = 0;
    bool is_mapped = false;

    while ((status=gt_input_generic_parser_get_template(buffered_input,template,generic_parser_attributes))) {
      // calculate counters such that splits are not taken into account
      gt_template_recalculate_counters_no_splits(template);
      gt_template_sort_by_distance__score_no_split(template);

      is_mapped = gt_template_is_mapped(template);
      const bool is_unique = gt_template_get_num_mmaps(template) == 1;
      if(is_mapped && (!is_unique || !params->keep_unique )){
        gt_template *template_filtered = gt_template_dup(template,false,false);
        gt_template_filter(template_filtered,template, gtf, params);
        gt_template_delete(template);
        template = template_filtered;
      }

      if(gtf != NULL){
        gt_template *template_filtered = gt_template_dup(template,false,false);
        gt_annotation_filter(template_filtered, template, params, gtf);
        gt_template_delete(template);
        template = template_filtered;
        gt_template_recalculate_counters_no_splits(template);
      }

      // get back the original counters
      gt_template_recalculate_counters(template);
      gt_output_generic_bofprint_template(buffered_output,template,generic_printer_attributes);

    }
    gt_template_delete(template);
    gt_buffered_input_file_close(buffered_input);
    gt_buffered_output_file_close(buffered_output);
    gt_generic_printer_attributes_delete(generic_printer_attributes);
    gt_input_generic_parser_attributes_delete(generic_parser_attributes);
  }

  if(params->close_output){
    gt_output_file_close(output);
  }
}
