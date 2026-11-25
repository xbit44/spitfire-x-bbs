// Limit complete publication units, not individual files. A payload and its
// TIC can sort far apart in the ordinary Native queue, and a blocked partner
// must not starve later complete units. Native's ACK-gated query is unchanged.
if(c.settings.mode==app::FtnMailerMode::external_binkd){
    if(maximum<2)throw StorageFailure(app::StorageErrorCode::invalid_argument,"External mail batches require room for a payload and TIC",SQLITE_MISUSE);
    sql=R"SQL(WITH eligible AS (
 SELECT * FROM sfx_ftn_outbound o WHERE network=?1 AND link=?2 AND ?7=1 AND
 ((state IN(1,2) AND generation=?3 AND claim_id=?4) OR
  (state=0 AND (claim_id IS NULL OR claim_id<>?4) AND (?5=1 OR flavor<>1)
   AND NOT EXISTS(SELECT 1 FROM sfx_ftn_job_echoes e JOIN sfx_ftn_link_state p
    ON p.network=o.network AND p.link=o.link WHERE e.job_id=o.job_id AND p.paused=1)))
), complete AS (
 SELECT e.*,coalesce(f.payload_job,e.job_id) AS unit FROM eligible e
 LEFT JOIN sfx_ftn_file_job_echoes f ON f.job_id=e.job_id
 WHERE NOT EXISTS(
  SELECT 1 FROM sfx_ftn_file_job_echoes d JOIN sfx_ftn_outbound p ON p.job_id=d.payload_job
  WHERE d.job_id=e.job_id AND p.state NOT IN(3,5) AND NOT EXISTS(SELECT 1 FROM eligible x WHERE x.job_id=p.job_id)
 ) AND NOT EXISTS(
  SELECT 1 FROM sfx_ftn_file_job_echoes d JOIN sfx_ftn_outbound t ON t.job_id=d.job_id
  WHERE d.payload_job=e.job_id AND t.state NOT IN(3,5) AND NOT EXISTS(SELECT 1 FROM eligible x WHERE x.job_id=t.job_id)
 )
), units AS (
 SELECT unit,count(*) AS files,max(flavor) AS priority,min(created_utc) AS oldest FROM complete GROUP BY unit
), page AS (
 SELECT unit,sum(files) OVER(ORDER BY priority DESC,oldest,unit ROWS UNBOUNDED PRECEDING) AS total FROM units
)
SELECT )SQL"+std::string(ftn_work_detail::job_columns)+
        " FROM complete WHERE unit IN(SELECT unit FROM page WHERE total<=?6) ORDER BY flavor DESC,created_utc,ordinal,job_id";
}
